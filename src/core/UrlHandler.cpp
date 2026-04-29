#include "core/UrlHandler.h"

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

void UrlHandler::install(QCoreApplication* app)
{
    if (app) app->installEventFilter(this);

    // Clean any stale socket left behind by a previous crash, then listen.
    QLocalServer::removeServer(socketName());
    m_server = new QLocalServer(this);
    if (!m_server->listen(socketName())) {
        qWarning() << "UrlHandler: QLocalServer failed to listen:"
                   << m_server->errorString();
        // Non-fatal: we just lose the single-instance / out-of-process URL
        // forwarding feature. In-process URL clicks still work.
        return;
    }
    connect(m_server, &QLocalServer::newConnection,
            this, &UrlHandler::onNewConnection);
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

void UrlHandler::checkAndroidShareIntent()
{
#ifdef Q_OS_ANDROID
    QJniObject activity(QNativeInterface::QAndroidApplication::context());
    if (!activity.isValid()) return;
    QJniObject intent = activity.callObjectMethod(
        "getIntent", "()Landroid/content/Intent;");
    if (!intent.isValid()) return;

    QJniObject actionObj = intent.callObjectMethod(
        "getAction", "()Ljava/lang/String;");
    if (!actionObj.isValid()) return;
    QString action = actionObj.toString();
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
        // Clear the action so a rotate/resume doesn't re-fire it.
        intent.callObjectMethod("setAction",
            "(Ljava/lang/String;)Landroid/content/Intent;",
            QJniObject::fromString("").object<jstring>());
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
    intent.callObjectMethod("setAction",
        "(Ljava/lang/String;)Landroid/content/Intent;",
        QJniObject::fromString("").object<jstring>());
#endif
}
