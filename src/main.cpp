#include <QApplication>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickStyle>
#include <QWindow>

#include "core/App.h"
#include "core/AppProfile.h"
#include "core/NotificationManager.h"
#include "core/Settings.h"
#include "core/UrlHandler.h"
// Mobile routes an inbound URL through the sign-in before treating it as a
// deep link; see the urlReceived connection below. The header decides whether
// this platform does that (BSFCHAT_NATIVE_OIDC_REDIRECT).
#include "identity/IdentityClient.h"
#include "net/ServerManager.h"
#include "net/ServerConnection.h"
#include "core/TintedIconProvider.h"
#include "core/MediaDownloader.h"
#include "core/Haptics.h"
#include "core/MobileKeyboard.h"
#include "core/Updater.h"
#include "core/AndroidPermissions.h"
#include "util/FileLogger.h"
// Mirror the platform gate below where these get instantiated.
// Keep the includes paired with the use sites so a platform-gate
// edit only has to be made in one place to widen support.
//
// BSFCHAT_VOICE_ENABLED belongs in this gate as much as the platform
// does: ScreenShareController.cpp is compiled only inside the
// `if(BSFCHAT_ENABLE_VOICE)` block in CMakeLists.txt, so without it a
// voice-off desktop build declares a controller that links to nothing.
// Voice-off is a shipped configuration, not a hypothetical — iOS
// defaults BSFCHAT_ENABLE_VOICE to OFF (see CMakeLists.txt).
//
// voice/VoiceEngine.h used to be included here unconditionally, and
// nothing in this file has ever named VoiceEngine — main() only touches
// the screen-share and camera controllers. It pulls in rtc/rtc.hpp, so
// `-DBSFCHAT_ENABLE_VOICE=OFF` died at the first translation unit.
// Deleted rather than gated: an include with no use site is not
// something worth keeping correct.
#if defined(BSFCHAT_VOICE_ENABLED) \
    && (defined(Q_OS_MACOS) || defined(Q_OS_WIN) \
        || (defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)))
#include "voice/ScreenShareController.h"
#endif
#if defined(Q_OS_ANDROID) && defined(BSFCHAT_VOICE_ENABLED)
#include "voice/AndroidScreenShareController.h"
#endif
#if defined(BSFCHAT_VOICE_ENABLED)
// CameraController uses QCamera on every platform except macOS
// (which has a native Objective-C++ wrapper to work around Qt
// Multimedia's AVFoundation quirks). Header is portable.
#include "voice/CameraController.h"
#endif

#if defined(__GNUC__) || defined(__clang__)
__attribute__((used))
#endif
static const char g_build_tag[] = "Bullshit Free Chat";

int main(int argc, char *argv[])
{
    // QML's XMLHttpRequest refuses qrc:// URLs (treats them as
    // "local file read") unless this env var is set. Icon.qml uses
    // XHR to fetch SVG source text, substitute `currentColor`, and
    // load via data URL — that's the only way to tint SVGs on the
    // Qt 6.5 Android backend (MultiEffect / ColorOverlay both
    // silently fail there).
    qputenv("QML_XHR_ALLOW_FILE_READ", "1");

    // Resolve `--profile <name>` / $BSFCHAT_PROFILE before anything else:
    // the single-instance handoff below already needs the profile-specific
    // socket name, and it runs before QApplication exists. With no profile
    // this is a no-op and every name below keeps its historical value.
    bsfchat::setActiveProfile(
        bsfchat::profileFromArguments(argc, argv, qgetenv("BSFCHAT_PROFILE")));

    // Parse any `bsfchat://…` URL the OS passed on the command line BEFORE
    // constructing QGuiApplication so a forwarded URL doesn't pay the cost
    // of spinning up Qt GUI subsystems in the forwarding process.
    const QString startupUrl = UrlHandler::urlFromArgv(argc, argv);

    // Windows/Linux: if the OS is launching us just to hand a URL to an
    // already-running instance, forward it over QLocalSocket and exit.
    // macOS: Launch Services already reuses the running app, so argv won't
    // carry a URL there — this path is a no-op.
    if (!startupUrl.isEmpty()
        && UrlHandler::forwardToRunningInstance(startupUrl)) {
        return 0;
    }

    // QApplication (not QGuiApplication) because QSystemTrayIcon lives in
    // QtWidgets and requires a QApplication event loop for its platform
    // integration. Everything else continues to use the QGuiApplication
    // API surface via QApplication's inheritance.
    QApplication app(argc, argv);
    // Both are "BSFChat" unless --profile/$BSFCHAT_PROFILE was given, in
    // which case the application name carries a "-<profile>" suffix. That
    // one suffix namespaces QSettings, QStandardPaths::AppDataLocation
    // (hence LocalCache) and the macOS log directory together.
    app.setApplicationName(bsfchat::applicationName());
    app.setOrganizationName(bsfchat::organizationName());
    QQuickStyle::setStyle("Basic");

    // Mirror all logging to a rotating file. A Finder-launched app has
    // no stderr, so without this the client leaves no trace to debug
    // from. Installed after setApplicationName so the log dir resolves.
    bsfchat::installFileLogger();
    qInfo() << "BSFChat starting, logging to" << bsfchat::logDirectory();
    if (!bsfchat::activeProfile().isEmpty())
        qInfo() << "Profile:" << bsfchat::activeProfile()
                << "(application name" << bsfchat::applicationName() << ")";

    App application;

    // Theme.qml is a QML singleton and singletons don't see context
    // properties, so expose Settings as a typed QML singleton too. Regular
    // QML files continue to use the lowercase `appSettings` context
    // property; Theme.qml uses the uppercase `AppSettings` singleton.
    qmlRegisterSingletonInstance<Settings>("BSFChat", 1, 0, "AppSettings",
                                           application.settings());
    // Don't let QML take ownership of the Settings instance (App owns it).
    QQmlEngine::setObjectOwnership(application.settings(),
                                   QQmlEngine::CppOwnership);

    // Single-instance + scheme registration + macOS QFileOpenEvent filter.
    UrlHandler urlHandler;
    urlHandler.install(&app);
    // Only the default profile claims the OS-level bsfchat:// handler. A
    // secondary `--profile` instance is a second copy of the same app on
    // one machine; letting it rewrite the system registration would point
    // every link at whichever test profile was started last.
    if (bsfchat::activeProfile().isEmpty())
        urlHandler.registerSchemeHandler();

    // Funnel every inbound URL into ServerManager and raise/activate the
    // window so the user sees the navigation happen.
    QObject::connect(&urlHandler, &UrlHandler::urlReceived,
                     &app, [&application](const QString& url) {
#ifdef BSFCHAT_NATIVE_OIDC_REDIRECT
        // A `bsfchat://oauth/callback?…` is a sign-in reply, not a link to a
        // message, and it must reach the sign-in rather than openMessageLink
        // — which would go looking for a room called "oauth" and leave the
        // login spinner running forever.
        //
        // On Android this IS the sign-in's delivery path. On iOS
        // ASWebAuthenticationSession normally intercepts the redirect before
        // the OS sees it, so this only fires if iOS routes it to us anyway.
        // Either way, every other bsfchat:// URL falls through untouched.
        if (IdentityClient::deliverCallbackUrl(url)) return;
#endif
        application.serverManager()->openMessageLink(url);
        for (QWindow* w : QGuiApplication::topLevelWindows()) {
            w->raise();
            w->requestActivate();
        }
    });

    QQmlApplicationEngine engine;
    // Custom image provider for theme-tinted SVG icons. Resolves
    // `image://tinted/<name>/<hex>` by rasterising the qrc-packed
    // SVG with strokes painted in the requested colour. Icon.qml
    // routes every call through here rather than the native Qt
    // SVG + MultiEffect path which fails on Android arm64 GL.
    engine.addImageProvider(QStringLiteral("tinted"),
                            new TintedIconProvider);

    // MediaDownloader — HTTP URL → local cached file. On Android the
    // system MediaPlayer can't reliably stream several common server-
    // hosted containers (Matroska in particular), so VideoPlayerCard
    // on mobile downloads first and hands the player a file:// URL.
    MediaDownloader mediaDownloader;
    QQmlEngine::setObjectOwnership(&mediaDownloader, QQmlEngine::CppOwnership);
    engine.rootContext()->setContextProperty("mediaDownloader",
                                             &mediaDownloader);

    // Haptics — short platform buzz on long-press + swipe commit.
    // Safe to expose on all platforms; methods no-op on desktop.
    Haptics haptics;
    QQmlEngine::setObjectOwnership(&haptics, QQmlEngine::CppOwnership);
    engine.rootContext()->setContextProperty("haptics", &haptics);

    // Software-keyboard bridge. The mobile shell pushes its own layout
    // clear of the keyboard and needs to know what the platform did on
    // its own account, so the two do not both move the composer. Exposed
    // on every platform (it reads 0 and does nothing off iOS) so
    // MobileMain does not have to guard every call site.
    MobileKeyboard mobileKeyboard;
    QQmlEngine::setObjectOwnership(&mobileKeyboard, QQmlEngine::CppOwnership);
    engine.rootContext()->setContextProperty("mobileKeyboard", &mobileKeyboard);

    // Auto-update — desktop only. We don't ship the dialog into the
    // mobile QML on Android / iOS because (a) Play Store / TestFlight
    // own the update mechanism, and (b) the platform apply paths
    // don't make sense for a sandboxed install.
#if !defined(Q_OS_ANDROID) && !defined(Q_OS_IOS)
    Updater updater;
    QQmlEngine::setObjectOwnership(&updater, QQmlEngine::CppOwnership);
    engine.rootContext()->setContextProperty("updater", &updater);
    if (application.settings()
        && application.settings()->autoUpdateCheck()) {
        updater.startAutoCheckSchedule();
    }
#endif

    // Android runtime-permission bridge — QML calls
    // `androidPerms.requestMicrophone()` before joining voice and
    // binds to `microphoneResult` for the allow/deny answer. No-op on
    // desktop (methods short-circuit to "granted").
    AndroidPermissions androidPerms;
    QQmlEngine::setObjectOwnership(&androidPerms, QQmlEngine::CppOwnership);
    engine.rootContext()->setContextProperty("androidPerms", &androidPerms);

    // UrlHandler exposes two signals — `urlReceived` for
    // `bsfchat://…` deep-links and `sharedPayloadReceived` for
    // Android ACTION_SEND shares — that QML listens to via a
    // Connections block.
    QQmlEngine::setObjectOwnership(&urlHandler, QQmlEngine::CppOwnership);
    engine.rootContext()->setContextProperty("urlHandler", &urlHandler);

    // When the Android activity gets a fresh intent while already
    // running (singleTop relaunch), BSFChatActivity.java's
    // onNewIntent() calls into nativeOnNewIntent() which surfaces
    // here as AndroidPermissions::newIntentReceived. Re-run the
    // intent extraction so warm shares aren't lost — and, since the
    // OIDC redirect comes back as an ACTION_VIEW intent on Android,
    // so a sign-in returning from the browser is picked up at all.
    QObject::connect(&androidPerms, &AndroidPermissions::newIntentReceived,
        &urlHandler, [&urlHandler]() {
            urlHandler.checkAndroidLaunchIntent();
        });

    engine.rootContext()->setContextProperty("serverManager", application.serverManager());
    engine.rootContext()->setContextProperty("appSettings", application.settings());
    // Screen-share controller binds for every desktop platform that
    // has voice compiled in (ScreenShareController.cpp is voice-gated
    // in CMakeLists, and the capture feeds the voice transport).
    // ScreenShareController internally branches on the build target:
    // macOS uses our CG/ScreenCaptureKit-backed MacScreenCapturer
    // (Homebrew Qt ships without QT_FEATURE_screen_capture), while
    // Windows + Linux use Qt's QScreenCapture against the bundled
    // FFmpeg multimedia backend. iOS / Android need entirely
    // different capture paths (ReplayKit / MediaProjection) — the
    // Android one lives in its own #if block below; iOS hasn't been
    // wired yet so the QML button stays hidden there via the
    // `typeof screenShare !== "undefined"` check in VoiceDock.
#if defined(BSFCHAT_VOICE_ENABLED) \
    && (defined(Q_OS_MACOS) || defined(Q_OS_WIN) \
        || (defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)))
    ScreenShareController screenShare;
    QQmlEngine::setObjectOwnership(&screenShare, QQmlEngine::CppOwnership);
    engine.rootContext()->setContextProperty("screenShare", &screenShare);
    screenShare.setServerManager(application.serverManager());
    screenShare.setSettings(application.settings());
#endif
#if defined(Q_OS_ANDROID) && defined(BSFCHAT_VOICE_ENABLED)
    // Parallel screen-share controller for Android. Exposes the
    // same `screenShare` context property the desktop uses so
    // VoiceDock's button binds unchanged. Internally routes to
    // MediaProjection via JNI — see AndroidScreenShareController
    // and ScreenCaptureHelper.java.
    AndroidScreenShareController screenShare;
    QQmlEngine::setObjectOwnership(&screenShare, QQmlEngine::CppOwnership);
    engine.rootContext()->setContextProperty("screenShare", &screenShare);
    screenShare.setServerManager(application.serverManager());
    screenShare.setSettings(application.settings());
#endif
#if defined(BSFCHAT_VOICE_ENABLED)
    // CameraController is portable — QCamera works on desktop
    // (non-macOS fork path), macOS (native MacCameraCapturer
    // inside the class), and Android in Qt 6.5+. Same context
    // property name across all three so VoiceDock binds
    // unchanged; runtime permission is gated by
    // androidPerms.requestCamera on the QML side.
    CameraController camera;
    QQmlEngine::setObjectOwnership(&camera, QQmlEngine::CppOwnership);
    engine.rootContext()->setContextProperty("camera", &camera);
    camera.setServerManager(application.serverManager());
    camera.setSettings(application.settings());
#endif
#if defined(BSFCHAT_VOICE_ENABLED) && !defined(Q_OS_IOS)
    // Explicit media announcement — mirror screen-share / camera
    // activity into the voice server's member state (PUT voice/state
    // {screen_sharing, camera_on}) so remote clients learn about a
    // share up front instead of inferring it from arriving frames.
    // Wired here because main() is the only scope that sees both
    // controllers at once. iOS is excluded until it grows a capture
    // controller (voice is off there by default anyway).
    {
        auto* mgr = application.serverManager();
        auto announceMediaState = [mgr, &screenShare, &camera]() {
            if (auto* vs = mgr->voiceServer())
                vs->setLocalMediaState(screenShare.active(), camera.active());
        };
        // Any active-state flip announces the new pair; no-op unless
        // some connection is in voice. Receiver is `camera` — declared
        // after screenShare, so it's destroyed first and both connects
        // auto-drop before either capture could dangle.
        QObject::connect(&screenShare, &decltype(screenShare)::activeChanged,
                         &camera, announceMediaState);
        QObject::connect(&camera, &CameraController::activeChanged,
                         &camera, announceMediaState);
        // A voice join that happens while a controller is already live
        // announces once — the server starts every fresh membership
        // with both flags false, so without this the share would stay
        // invisible until the next toggle.
        auto wireJoinAnnounce = [mgr, &screenShare, &camera,
                                 announceMediaState](int index) {
            auto* sc = mgr->connectionAt(index);
            if (!sc) return;
            auto announceIfCapturing =
                [sc, &screenShare, &camera, announceMediaState]() {
                if (sc->inVoiceChannel()
                    && (screenShare.active() || camera.active()))
                    announceMediaState();
            };
            QObject::connect(sc, &ServerConnection::activeVoiceRoomIdChanged,
                             &camera, announceIfCapturing);
            // Second edge, same need: the ghost reaper retired our voice
            // row and the V-H2 re-join was accepted. The room and the
            // engine are unchanged — so activeVoiceRoomIdChanged does NOT
            // fire — but the membership is new and the server starts
            // every membership with screen_sharing and camera_on false.
            // Without re-announcing here, a share that was live when the
            // reaper fired vanishes from everyone else's roster and only
            // comes back if the user toggles it off and on.
            QObject::connect(sc, &ServerConnection::voiceMembershipRenewed,
                             &camera, announceIfCapturing);
        };
        for (int i = 0; i < mgr->connectionCount(); ++i) wireJoinAnnounce(i);
        QObject::connect(mgr, &ServerManager::serverAdded,
                         &camera, wireJoinAnnounce);
    }
#endif
    // Mobile builds (iOS / Android) load a phone-native shell that
    // wraps the same leaf components in a drawer-based navigation
    // instead of the desktop's three-panel layout. Everything below
    // the chrome — ServerSidebar, ChannelList, MessageView, etc. —
    // is shared 1:1.
#if defined(Q_OS_IOS) || defined(Q_OS_ANDROID)
    engine.load(QUrl(QStringLiteral(
        "qrc:/qt/qml/BSFChat/qml/mobile/MobileMain.qml")));
#else
    engine.load(QUrl(QStringLiteral("qrc:/qt/qml/BSFChat/qml/main.qml")));
#endif

    if (engine.rootObjects().isEmpty())
        return -1;

    // Teach the NotificationManager which window is "the app" so it can
    // skip notifications when the user is already looking at the active
    // room, and raise+activate the correct window on notification click.
    if (auto* nm = application.notificationManager()) {
        QObject* root = engine.rootObjects().first();
        nm->setWindow(qobject_cast<QWindow*>(root));
    }

    // If a URL was passed on the command line and we're the first instance,
    // fire it once the event loop starts so the handlers above process it
    // after the window has been created.
    if (!startupUrl.isEmpty()) {
        QMetaObject::invokeMethod(&urlHandler, [&urlHandler, startupUrl]() {
            emit urlHandler.urlReceived(startupUrl);
        }, Qt::QueuedConnection);
    }

    // Android launch-intent pickup — if the app was launched by another
    // app via "Share to BSFChat", or by a bsfchat:// link, dispatch it
    // after QML is loaded so a Connections block on the root can drop the
    // payload into the active channel.
    QMetaObject::invokeMethod(&urlHandler, [&urlHandler]() {
        urlHandler.checkAndroidLaunchIntent();
    }, Qt::QueuedConnection);

    // Clean shutdown — make sure we drop any in-flight voice session
    // before the process exits. Without this the server keeps the
    // client listed as present in the voice room until its ICE
    // timeout catches up (30+ seconds, sometimes longer on Android
    // where the OS may have killed us instantly after backgrounding).
    QObject::connect(&app, &QCoreApplication::aboutToQuit, &app,
        [sm = application.serverManager()]() {
            if (sm) sm->leaveAllVoice();
        });

    // NOTE: we used to leave voice on Android
    // applicationStateChanged → Suspended/Hidden, but that's wrong
    // policy for a voice call — users expect to keep talking with
    // the screen off or while switching apps. The foreground
    // service anchored by VoiceService.java keeps the process
    // alive across backgrounding, so `aboutToQuit` is enough.

    return app.exec();
}
