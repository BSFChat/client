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

#if defined(__GNUC__) || defined(__clang__)
__attribute__((used))
#endif
static const char g_build_tag[] = "Bullshit Free Chat";

int main(int argc, char *argv[])
{
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
    engine.rootContext()->setContextProperty("serverManager", application.serverManager());
    engine.rootContext()->setContextProperty("appSettings", application.settings());
    engine.load(QUrl(QStringLiteral("qrc:/qt/qml/BSFChat/qml/main.qml")));

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

    return app.exec();
}
