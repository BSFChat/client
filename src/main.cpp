#include <QApplication>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickStyle>
#include <QWindow>

#include "core/App.h"
#include "core/NotificationManager.h"
#include "core/Settings.h"
#include "core/UrlHandler.h"
#include "net/ServerManager.h"
#include "net/ServerConnection.h"
#include "core/TintedIconProvider.h"
#include "core/MediaDownloader.h"
#include "core/Haptics.h"
#include "core/AndroidPermissions.h"
#if defined(Q_OS_MACOS) && !defined(Q_OS_IOS)
#include "voice/ScreenShareController.h"
#include "voice/VoiceEngine.h"
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
    app.setApplicationName("BSFChat");
    app.setOrganizationName("BSFChat");
    QQuickStyle::setStyle("Basic");

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
    urlHandler.registerSchemeHandler();

    // Funnel every inbound URL into ServerManager and raise/activate the
    // window so the user sees the navigation happen.
    QObject::connect(&urlHandler, &UrlHandler::urlReceived,
                     &app, [&application](const QString& url) {
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

    // When the Android activity gets a fresh ACTION_SEND while
    // already running (singleTop relaunch), BSFChatActivity.java's
    // onNewIntent() calls into nativeOnNewIntent() which surfaces
    // here as AndroidPermissions::newIntentReceived. Re-run the
    // share-intent extraction so warm shares aren't lost.
    QObject::connect(&androidPerms, &AndroidPermissions::newIntentReceived,
        &urlHandler, [&urlHandler]() {
            urlHandler.checkAndroidShareIntent();
        });

    engine.rootContext()->setContextProperty("serverManager", application.serverManager());
    engine.rootContext()->setContextProperty("appSettings", application.settings());
    // Screen-share + webcam controllers are desktop-macOS only for
    // now. iOS / Android need different capture paths (platform
    // media APIs + mobile-appropriate UX) that haven't been wired
    // yet. Gating keeps the mobile binary lighter and avoids
    // shipping disabled buttons.
#if defined(Q_OS_MACOS) && !defined(Q_OS_IOS)
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

    // Android share-intent pickup — if the app was launched by
    // another app via "Share to BSFChat", fire the share signal
    // after QML is loaded so a Connections block on the root can
    // drop the payload into the active channel.
    QMetaObject::invokeMethod(&urlHandler, [&urlHandler]() {
        urlHandler.checkAndroidShareIntent();
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
