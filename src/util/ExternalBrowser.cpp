#include "util/ExternalBrowser.h"

#include <QDebug>
#include <QDesktopServices>
#include <QProcess>
#include <QStandardPaths>

namespace bsfchat {
namespace {

OpenUrlHandler g_testHandler;

#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
// Launchers that take the URL as their one and only argument. `gio open` is
// deliberately absent: it needs a subcommand, and anything that needs argv
// shaping belongs in QDesktopServices' detection rather than here.
//
// xdg-open first — it is the one that respects the user's default-browser
// setting. The rest are the fallbacks Qt itself tries, repeated here because
// we are bypassing Qt's launcher for the environment's sake, not because we
// think we can detect better than it can.
const char* const kOpeners[] = {
    "xdg-open", "x-www-browser", "sensible-browser",
    "firefox",  "google-chrome", "chromium-browser", "chromium",
};

QString findOpener()
{
    for (const char* name : kOpeners) {
        const QString path = QStandardPaths::findExecutable(QString::fromLatin1(name));
        if (!path.isEmpty()) return path;
    }
    return QString();
}
#endif

// The query carries `state`, `nonce` and the PKCE challenge. None of them is
// a secret the way the verifier is, but a log file gets pasted into issues,
// so the log gets the endpoint and the user gets the full link.
QString forLog(const QUrl& url)
{
    return url.toString(QUrl::RemoveQuery | QUrl::RemoveUserInfo);
}

} // namespace

QStringList sanitizedLaunchArgv(const QString& hostLdLibraryPath,
                                const QString& opener, const QUrl& url)
{
    QStringList argv;
    // Qt's own path variables would send a Qt-based browser (Falkon, Konqueror)
    // looking for plugins and QML in OUR bundle.
    argv << QStringLiteral("-u") << QStringLiteral("QT_PLUGIN_PATH")
         << QStringLiteral("-u") << QStringLiteral("QML2_IMPORT_PATH")
         << QStringLiteral("-u") << QStringLiteral("QML_IMPORT_PATH");
    if (hostLdLibraryPath.isEmpty())
        argv << QStringLiteral("-u") << QStringLiteral("LD_LIBRARY_PATH");
    else
        argv << (QStringLiteral("LD_LIBRARY_PATH=") + hostLdLibraryPath);
    argv << opener << url.toString();
    return argv;
}

void setOpenUrlHandlerForTesting(OpenUrlHandler handler)
{
    g_testHandler = std::move(handler);
}

bool openExternalUrl(const QUrl& url)
{
    if (g_testHandler) return g_testHandler(url);

    if (!url.isValid() || url.isEmpty()) {
        qWarning() << "[browser] refusing to open an invalid URL";
        return false;
    }

#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
    // Only when the release launcher started us. A locally built app, or one
    // packaged by a distribution, has an environment its browser can live
    // with and should go through Qt (which also knows about portals).
    if (!qEnvironmentVariableIsEmpty("BSFCHAT_BUNDLE_DIR")) {
        const QString envTool = QStandardPaths::findExecutable(QStringLiteral("env"));
        const QString opener = findOpener();
        if (envTool.isEmpty() || opener.isEmpty()) {
            qWarning() << "[browser] bundled run: no" << (envTool.isEmpty() ? "env" : "browser launcher")
                       << "on PATH; falling back to the desktop's own handler";
        } else {
            const QString hostLd =
                QString::fromLocal8Bit(qgetenv("BSFCHAT_HOST_LD_LIBRARY_PATH"));
            if (QProcess::startDetached(envTool, sanitizedLaunchArgv(hostLd, opener, url))) {
                qInfo() << "[browser] opened" << forLog(url) << "via" << opener;
                return true;
            }
            qWarning() << "[browser]" << opener << "could not be started for"
                       << forLog(url);
        }
    }
#endif

    if (QDesktopServices::openUrl(url)) {
        qInfo() << "[browser] opened" << forLog(url);
        return true;
    }

    // Qt has already logged its own reason ("Unable to detect a web browser
    // to launch…", or a failed launch). This line is the one that names the
    // app-level consequence, so a log search for the feature finds it.
    qWarning() << "[browser] the desktop refused to open" << forLog(url)
               << "— nothing was launched";
    return false;
}

} // namespace bsfchat
