#include "core/UrlHandler.h"

#include "core/AppProfile.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileOpenEvent>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QStandardPaths>
#include <QString>

#ifdef Q_OS_WIN
#  include <QSettings>
#endif

#ifdef Q_OS_ANDROID
#  include <QJniEnvironment>
#  include <QJniObject>
#endif

namespace {
// Per-user socket so two accounts on the same machine don't step on each
// other. Qt normalizes this to %TEMP% / /tmp / XDG_RUNTIME_DIR as appropriate.
constexpr const char* kSocketTail = "bsfchat-url-ipc";
} // namespace

QString UrlHandler::socketName()
{
    // QStandardPaths(RuntimeLocation) returns "" on some platforms; fall
    // back to the binary name — QLocalServer handles the platform details.
    QString base = QStringLiteral("%1-%2")
                       .arg(QString::fromLatin1(kSocketTail),
                            QString::fromLocal8Bit(qgetenv("USER").isEmpty()
                                                      ? qgetenv("USERNAME")
                                                      : qgetenv("USER")));
    if (base.endsWith('-')) base.chop(1);
    // Per-profile socket as well as per-user: the QLocalServer below is the
    // single-instance lock, so without this a second `--profile` client
    // would hand its URL to the first one and exit instead of starting.
    // Empty for the default profile, so the historical name is unchanged.
    base += bsfchat::socketSuffixForProfile(bsfchat::activeProfile());
    return base;
}

// ---- Static helpers ----------------------------------------------------

QString UrlHandler::urlFromArgv(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i) {
        QString a = QString::fromLocal8Bit(argv[i]);
        if (a.startsWith(QStringLiteral("bsfchat://"))) return a;
    }
    return {};
}

bool UrlHandler::forwardToRunningInstance(const QString& url)
{
    if (url.isEmpty()) return false;

    QLocalSocket socket;
    socket.connectToServer(socketName());
    // 300ms is generous for a local socket — the server-side accept is
    // effectively instantaneous on a running app. If we time out, assume
    // no running instance.
    if (!socket.waitForConnected(300)) return false;

    QByteArray payload = url.toUtf8() + '\n';
    socket.write(payload);
    if (!socket.waitForBytesWritten(300)) {
        socket.disconnectFromServer();
        return false;
    }
    socket.disconnectFromServer();
    return true;
}

// ---- Instance API -------------------------------------------------------

UrlHandler::UrlHandler(QObject* parent)
    : QObject(parent)
{
}

UrlHandler::~UrlHandler() = default;

UrlHandler::Acquisition UrlHandler::acquireServer(QLocalServer* server,
                                                  const QString& name)
{
    if (!server || name.isEmpty()) return Acquisition::Failed;

    // Ask first, bind second. The order matters per platform:
    //   * Windows: QLocalServer is a named pipe, and a second listen() on a
    //     name that is already served SUCCEEDS (pipes allow multiple
    //     instances). A bind-first design therefore cannot detect a live
    //     instance there at all — that is what failed the rc.10 Windows
    //     job — so the probe has to come first.
    //   * Unix: a live listener answers the probe; a stale socket file
    //     (crash, SIGKILL, a reboot that kept /tmp) refuses both the probe
    //     and the bind, and only then is unlinking it safe.
    // A probe against a free name fails immediately on both platforms
    // (ENOENT / ERROR_FILE_NOT_FOUND), so this costs a clean launch nothing.
    {
        QLocalSocket probe;
        probe.connectToServer(name);
        if (probe.waitForConnected(300)) {
            probe.disconnectFromServer();
            return Acquisition::AnotherInstance;
        }
    }

    if (server->listen(name)) return Acquisition::Listening;

    // Nobody answered and the bind still failed: a stale socket file.
    // Unlinking it is safe now, and only now.
    QLocalServer::removeServer(name);
    if (server->listen(name)) return Acquisition::Listening;
    return Acquisition::Failed;
}

void UrlHandler::install(QCoreApplication* app)
{
    if (app) app->installEventFilter(this);

    m_server = new QLocalServer(this);
    switch (acquireServer(m_server, socketName())) {
    case Acquisition::Listening:
        connect(m_server, &QLocalServer::newConnection,
                this, &UrlHandler::onNewConnection);
        return;
    case Acquisition::AnotherInstance:
        // Another live instance of this profile owns URL forwarding. We do
        // not take it from them: deep links keep reaching the process that
        // has been running, and this one still handles in-process clicks.
        // Run a second account with --profile to get its own socket.
        qInfo() << "UrlHandler: another instance is already handling "
                   "bsfchat:// links for this profile; leaving it in place";
        delete m_server;
        m_server = nullptr;
        return;
    case Acquisition::Failed:
        qWarning() << "UrlHandler: QLocalServer failed to listen:"
                   << m_server->errorString();
        // Non-fatal: we just lose the out-of-process URL forwarding
        // feature. In-process URL clicks still work.
        delete m_server;
        m_server = nullptr;
        return;
    }
}

void UrlHandler::onNewConnection()
{
    while (auto* client = m_server->nextPendingConnection()) {
        connect(client, &QLocalSocket::readyRead, this, [this, client]() {
            // A well-behaved forwarder sends one line: "<url>\n". We accept
            // multi-line payloads in case we ever batch-forward.
            while (client->canReadLine()) {
                const QByteArray line = client->readLine().trimmed();
                if (!line.isEmpty())
                    emit urlReceived(QString::fromUtf8(line));
            }
        });
        connect(client, &QLocalSocket::disconnected,
                client, &QLocalSocket::deleteLater);
    }
}

bool UrlHandler::eventFilter(QObject* obj, QEvent* ev)
{
    // QFileOpenEvent is how macOS (and iOS) delivers URL activations from
    // Launch Services to a running Qt application.
    if (ev->type() == QEvent::FileOpen) {
        auto* foe = static_cast<QFileOpenEvent*>(ev);
        QString url = foe->url().toString();
        if (url.startsWith(QStringLiteral("bsfchat://"))) {
            emit urlReceived(url);
            return true;
        }
    }
    return QObject::eventFilter(obj, ev);
}

// ---- OS-level registration ---------------------------------------------

void UrlHandler::registerSchemeHandler()
{
#if defined(Q_OS_WIN)
    // HKCU registration is per-user and doesn't need admin. Writing on
    // every launch keeps the keys in sync if the install path moves.
    const QString exe = QDir::toNativeSeparators(
        QFileInfo(QCoreApplication::applicationFilePath()).absoluteFilePath());
    const QString cmd = QStringLiteral("\"%1\" \"%2\"")
                            .arg(exe, QStringLiteral("%1"));

    QSettings root("HKEY_CURRENT_USER\\Software\\Classes\\bsfchat",
                   QSettings::NativeFormat);
    root.setValue(".", "URL:BSFChat Protocol");
    root.setValue("URL Protocol", "");

    QSettings icon("HKEY_CURRENT_USER\\Software\\Classes\\bsfchat\\DefaultIcon",
                   QSettings::NativeFormat);
    icon.setValue(".", QStringLiteral("\"%1\",0").arg(exe));

    QSettings shell("HKEY_CURRENT_USER\\Software\\Classes\\bsfchat\\shell\\open\\command",
                    QSettings::NativeFormat);
    shell.setValue(".", cmd);

#elif defined(Q_OS_LINUX)
    // Emit a user-level .desktop file and wire it up via xdg-mime.
    // Idempotent: if the file is already present with matching Exec= line,
    // we skip the xdg-mime call (which is slow on some distros).
    const QString apps = QStandardPaths::writableLocation(
        QStandardPaths::ApplicationsLocation);
    if (apps.isEmpty()) return;
    QDir().mkpath(apps);

    const QString desktopPath = apps + QStringLiteral("/bsfchat.desktop");
    const QString exe = QFileInfo(QCoreApplication::applicationFilePath())
                            .absoluteFilePath();
    const QString desktopContents = QStringLiteral(
        "[Desktop Entry]\n"
        "Type=Application\n"
        "Name=BSFChat\n"
        "Comment=Bullshit Free Chat\n"
        "Exec=\"%1\" %u\n"
        "Terminal=false\n"
        "Categories=Network;InstantMessaging;\n"
        "MimeType=x-scheme-handler/bsfchat;\n"
        "NoDisplay=false\n").arg(exe);

    bool needsWrite = true;
    QFile existing(desktopPath);
    if (existing.exists() && existing.open(QIODevice::ReadOnly)) {
        needsWrite = (existing.readAll() != desktopContents.toUtf8());
        existing.close();
    }
    if (needsWrite) {
        QFile f(desktopPath);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            f.write(desktopContents.toUtf8());
            f.close();
            // Ask xdg-mime to register us as the default for the scheme.
            // Best-effort: if xdg-mime is missing, the .desktop file alone
            // is enough for most desktop environments to pick us up after
            // a database refresh.
            QProcess::startDetached("xdg-mime",
                {"default", "bsfchat.desktop", "x-scheme-handler/bsfchat"});
            QProcess::startDetached("update-desktop-database",
                {apps});
        }
    }
#else
    // macOS: CFBundleURLTypes is injected into Info.plist by the build
    // system (see client/CMakeLists.txt). Nothing to do at runtime.
#endif
}

void UrlHandler::checkAndroidLaunchIntent()
{
#ifdef Q_OS_ANDROID
    QJniObject activity(QNativeInterface::QAndroidApplication::context());
    if (!activity.isValid()) return;
    QJniObject intent = activity.callObjectMethod(
        "getIntent", "()Landroid/content/Intent;");
    if (!intent.isValid()) return;

    // Clearing the action after handling, here as for ACTION_SEND below, is
    // what stops a rotate or a resume replaying the same intent. It matters
    // more for a sign-in callback than for a share: an authorization code is
    // single-use at the provider, so a replay would fail the second exchange
    // and report an error over a sign-in that had already succeeded.
    const auto clearAction = [&intent]() {
        intent.callObjectMethod("setAction",
            "(Ljava/lang/String;)Landroid/content/Intent;",
            QJniObject::fromString("").object<jstring>());
    };

    QJniObject actionObj = intent.callObjectMethod(
        "getAction", "()Ljava/lang/String;");
    if (!actionObj.isValid()) return;
    QString action = actionObj.toString();

    // A `bsfchat://…` deep link — and, since the OIDC redirect uses the same
    // scheme on Android, the way a sign-in comes back. Android delivers no
    // QFileOpenEvent, so the eventFilter that handles this on macOS and iOS
    // never sees it; this is the equivalent.
    if (action == QStringLiteral("android.intent.action.VIEW")) {
        QJniObject dataObj = intent.callObjectMethod(
            "getDataString", "()Ljava/lang/String;");
        // Cleared unconditionally, including for a VIEW with no data: an
        // intent left with its action intact is re-read on every resume,
        // so an odd one would be re-examined forever.
        clearAction();
        if (!dataObj.isValid()) return;
        const QString url = dataObj.toString();
        if (url.startsWith(QStringLiteral("bsfchat://")))
            emit urlReceived(url);
        return;
    }

    if (action != QStringLiteral("android.intent.action.SEND")) return;

    // Grab the MIME type the source app declared so we can sniff
    // text-vs-binary paths.
    QJniObject typeObj = intent.callObjectMethod(
        "getType", "()Ljava/lang/String;");
    QString mime = typeObj.isValid() ? typeObj.toString() : QString();

    // text/* shares go through EXTRA_TEXT as a plain String.
    if (mime.startsWith("text/")) {
        QJniObject textKey = QJniObject::fromString(
            QStringLiteral("android.intent.extra.TEXT"));
        QJniObject text = intent.callObjectMethod(
            "getStringExtra",
            "(Ljava/lang/String;)Ljava/lang/String;",
            textKey.object<jstring>());
        if (text.isValid()) {
            emit sharedPayloadReceived(text.toString(), mime, false);
        }
        clearAction();
        return;
    }

    // Everything else (image/video/audio/application/*) arrives as an
    // EXTRA_STREAM Uri.
    QJniObject streamKey = QJniObject::fromString(
        QStringLiteral("android.intent.extra.STREAM"));
    QJniObject uri = intent.callObjectMethod(
        "getParcelableExtra",
        "(Ljava/lang/String;)Landroid/os/Parcelable;",
        streamKey.object<jstring>());
    if (!uri.isValid()) return;
    QString uriStr = uri.callObjectMethod(
        "toString", "()Ljava/lang/String;").toString();
    if (uriStr.isEmpty()) return;
    emit sharedPayloadReceived(uriStr, mime, true);
    clearAction();
#endif
}
