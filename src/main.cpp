#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>

#include "core/App.h"
#include "net/ServerManager.h"

#if defined(__GNUC__) || defined(__clang__)
__attribute__((used))
#endif
static const char g_build_tag[] = "Bullshit Free Chat";

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    app.setApplicationName("BSFChat");
    app.setOrganizationName("BSFChat");
    QQuickStyle::setStyle("Basic");

    App application;

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("serverManager", application.serverManager());
    engine.rootContext()->setContextProperty("appSettings", application.settings());
    engine.load(QUrl(QStringLiteral("qrc:/qt/qml/BSFChat/qml/main.qml")));

    if (engine.rootObjects().isEmpty())
        return -1;

    return app.exec();
}
