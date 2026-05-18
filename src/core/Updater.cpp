#include "Updater.h"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QStandardPaths>
#include <QUrl>
#include <QtGlobal>

#ifndef BSFCHAT_VERSION
#  define BSFCHAT_VERSION "0.0.0"
#endif

namespace {

// Compare two semver-ish strings (`major.minor.patch`, optional
// `-suffix` ignored). Returns >0 if a is newer than b, <0 if older,
// 0 equal. Tolerant of leading "v".
int compareSemver(const QString& aIn, const QString& bIn)
{
    auto strip = [](QString s) {
        if (s.startsWith('v') || s.startsWith('V')) s = s.mid(1);
        int dash = s.indexOf('-');
        if (dash >= 0) s = s.left(dash);
        return s;
    };
    QStringList ap = strip(aIn).split('.');
    QStringList bp = strip(bIn).split('.');
    int n = std::max(ap.size(), bp.size());
    for (int i = 0; i < n; ++i) {
        int av = i < ap.size() ? ap[i].toInt() : 0;
        int bv = i < bp.size() ? bp[i].toInt() : 0;
        if (av != bv) return av - bv;
    }
    return 0;
}

QString cacheDir()
{
    QString d = QStandardPaths::writableLocation(QStandardPaths::CacheLocation)
                + "/updates";
    QDir().mkpath(d);
    return d;
}

} // namespace

Updater::Updater(QObject* parent)
    : QObject(parent)
    , m_currentVersion(QStringLiteral(BSFCHAT_VERSION))
{
    m_recheckTimer.setInterval(6 * 60 * 60 * 1000); // 6 hours
    connect(&m_recheckTimer, &QTimer::timeout, this, &Updater::checkNow);
}

void Updater::setState(State s)
{
    if (m_state == s) return;
    m_state = s;
    emit stateChanged();
}

void Updater::setError(const QString& msg)
{
    m_lastError = msg;
    setState(Failed);
}

bool Updater::isNewer(const QString& candidate) const
{
    return compareSemver(candidate, m_currentVersion) > 0;
}

QString Updater::platformAssetSuffix() const
{
    // Match what `.github/workflows/ci.yml` actually publishes:
    //   BSFChat-macOS.dmg, BSFChat-Setup.exe, BSFChat-linux-x86_64.tar.gz
#if defined(Q_OS_MACOS)
    return QStringLiteral("BSFChat-macOS.dmg");
#elif defined(Q_OS_WIN)
    return QStringLiteral("BSFChat-Setup.exe");
#elif defined(Q_OS_LINUX)
    return QStringLiteral("BSFChat-linux-x86_64.tar.gz");
#else
    return QString();
#endif
}

void Updater::startAutoCheckSchedule()
{
    // 30s delay so the launch race (login, first /sync, QML compile)
    // is well clear of our network jitter.
    QTimer::singleShot(30 * 1000, this, &Updater::checkNow);
    m_recheckTimer.start();
}

void Updater::checkNow()
{
    if (m_state == Checking || m_state == Downloading
        || m_state == Applying) return;

    setState(Checking);
    QNetworkRequest req(QUrl(QStringLiteral(
        "https://api.github.com/repos/BSFChat/client/releases/latest")));
    req.setRawHeader("Accept", "application/vnd.github+json");
    req.setRawHeader("User-Agent", "BSFChat-Updater");
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    auto* reply = m_nam.get(req);
    connect(reply, &QNetworkReply::finished, this, &Updater::onCheckReply);
}

void Updater::onCheckReply()
{
    auto* reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply) return;
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        setError(QStringLiteral("Check failed: %1").arg(reply->errorString()));
        return;
    }
    QJsonParseError perr;
    QJsonDocument doc = QJsonDocument::fromJson(reply->readAll(), &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
        setError(QStringLiteral("Bad JSON from GitHub: %1").arg(perr.errorString()));
        return;
    }
    QJsonObject obj = doc.object();
    QString tag = obj.value("tag_name").toString();
    if (tag.isEmpty()) {
        setError(QStringLiteral("Latest release has no tag_name"));
        return;
    }
    if (!isNewer(tag)) {
        m_availableVersion.clear();
        setState(UpToDate);
        return;
    }

    // Newer tag found. Resolve our platform's asset URL.
    QString suffix = platformAssetSuffix();
    if (suffix.isEmpty()) {
        // Unsupported platform — leave as Idle so the UI doesn't
        // promise something we can't deliver.
        setState(Idle);
        return;
    }
    QJsonArray assets = obj.value("assets").toArray();
    QString matchedUrl;
    for (const auto& v : assets) {
        QJsonObject a = v.toObject();
        QString name = a.value("name").toString();
        if (name == suffix) {
            matchedUrl = a.value("browser_download_url").toString();
            break;
        }
    }
    if (matchedUrl.isEmpty()) {
        setError(QStringLiteral("Release %1 has no asset named %2")
                 .arg(tag, suffix));
        return;
    }

    m_availableVersion = tag;
    m_releaseHtmlUrl = obj.value("html_url").toString();
    m_releaseNotes = obj.value("body").toString();
    m_assetUrl = matchedUrl;
    setState(UpdateAvailable);
}

void Updater::downloadUpdate()
{
    if (m_state != UpdateAvailable) return;
    if (m_assetUrl.isEmpty()) return;

    // Cache asset under updates/<filename>; overwriting in place so
    // a re-download after a prior failed-write produces a fresh
    // artefact rather than a corrupt-half-of-each-version mix.
    QString fname = QFileInfo(QUrl(m_assetUrl).path()).fileName();
    if (fname.isEmpty()) fname = QStringLiteral("update.bin");
    m_assetLocalPath = cacheDir() + "/" + fname;

    QNetworkRequest req((QUrl(m_assetUrl)));
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setRawHeader("User-Agent", "BSFChat-Updater");
    auto* reply = m_nam.get(req);

    m_downloadedBytes = 0;
    m_totalBytes = 0;
    setState(Downloading);

    connect(reply, &QNetworkReply::downloadProgress,
            this, &Updater::onDownloadProgress);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            setError(QStringLiteral("Download failed: %1")
                     .arg(reply->errorString()));
            return;
        }
        QFile f(m_assetLocalPath);
        if (!f.open(QIODevice::WriteOnly)) {
            setError(QStringLiteral("Can't write to %1: %2")
                     .arg(m_assetLocalPath, f.errorString()));
            return;
        }
        f.write(reply->readAll());
        f.close();
        setState(ReadyToApply);
        emit updateReadyToInstall(m_availableVersion);
    });
}

void Updater::onDownloadProgress(qint64 received, qint64 total)
{
    m_downloadedBytes = received;
    m_totalBytes = total;
    emit progressChanged();
}

void Updater::applyUpdate()
{
    if (m_state != ReadyToApply) return;
    setState(Applying);

#if defined(Q_OS_MACOS)
    applyMac(m_assetLocalPath);
#elif defined(Q_OS_WIN)
    applyWindows(m_assetLocalPath);
#elif defined(Q_OS_LINUX)
    applyLinux(m_assetLocalPath);
#else
    setError(QStringLiteral("Unsupported platform"));
#endif
}

void Updater::openReleasePage()
{
    if (m_releaseHtmlUrl.isEmpty()) return;
    QDesktopServices::openUrl(QUrl(m_releaseHtmlUrl));
}

#if defined(Q_OS_MACOS)
void Updater::applyMac(const QString& dmgPath)
{
    // Spawn a detached shell helper that:
    //   1. waits for our PID to exit (so ditto can replace the binary)
    //   2. attaches the .dmg at a unique mountpoint
    //   3. finds the .app inside the DMG (case-insensitive — the
    //      bundle is bsfchat-app.app, not BSFChat.app; the previous
    //      -name 'BSFChat*.app' silently failed to match and turned
    //      the whole flow into a no-op, leaving the user on their
    //      old version)
    //   4. ditto's it over the currently-installed bundle
    //   5. detaches the DMG and relaunches the new app
    //
    // The whole script is wrapped in a logging block so future
    // failures show up in ~/Library/Logs/BSFChat/update.log instead
    // of disappearing into the detached-process void.
    QString appPath = QCoreApplication::applicationDirPath();
    // applicationDirPath = .../bsfchat-app.app/Contents/MacOS — climb to the .app
    QDir d(appPath);
    d.cdUp(); d.cdUp();
    QString bundlePath = d.absolutePath();

    // Unique mountpoint so a stale leftover or a parallel update
    // attempt can't collide on /Volumes/BSFChatUpdate.
    QString mountPt = QStringLiteral("/Volumes/BSFChatUpdate-%1")
        .arg(QCoreApplication::applicationPid());

    QString script = QStringLiteral(
        "LOG=\"$HOME/Library/Logs/BSFChat/update.log\"; "
        "mkdir -p \"$(dirname \"$LOG\")\"; "
        "exec >>\"$LOG\" 2>&1; "
        "echo; echo \"=== Update started $(date) ===\"; "
        "echo \"  pid_to_wait=%1 mount=%2 dmg=%3 dest=%4\"; "
        "while kill -0 %1 2>/dev/null; do sleep 0.2; done; "
        "echo \"  old process exited, mounting dmg\"; "
        "hdiutil attach -nobrowse -mountpoint %2 %3 || { echo \"ERROR: hdiutil attach failed\"; exit 1; }; "
        "SRC=$(find %2 -maxdepth 2 -iname '*.app' -type d | head -1); "
        "echo \"  src=$SRC\"; "
        "if [ -n \"$SRC\" ]; then "
        "  ditto \"$SRC\" %4; "
        "  echo \"  ditto exit=$?\"; "
        "else "
        "  echo \"ERROR: no .app found inside DMG\"; "
        "fi; "
        "hdiutil detach %2 -force; "
        "echo \"  relaunching %4\"; "
        "open %4; "
        "echo \"=== Update done $(date) ===\"")
        .arg(QCoreApplication::applicationPid())
        .arg(mountPt)
        .arg(dmgPath)
        .arg(bundlePath);

    QProcess::startDetached("/bin/bash", {"-c", script});
    QCoreApplication::quit();
}
#else
void Updater::applyMac(const QString&) {}
#endif

#if defined(Q_OS_WIN)
void Updater::applyWindows(const QString& exePath)
{
    // `/S` is NSIS's silent-install flag (must be uppercase and
    // first). installer.nsi taskkills any leftover bsfchat-app.exe,
    // overwrites in place using the InstallLocation reg key, and
    // relaunches us in its .onInstSuccess when ${Silent} is true.
    // Detached so this process can exit and free the binary.
    //
    // UAC: not silenceable. installer.nsi has
    // `RequestExecutionLevel admin`, so launching it from this
    // (non-elevated) process triggers the consent prompt before
    // the silent install can begin.
    QProcess::startDetached(exePath, {"/S"});
    QCoreApplication::quit();
}
#else
void Updater::applyWindows(const QString&) {}
#endif

#if defined(Q_OS_LINUX)
void Updater::applyLinux(const QString& tarPath)
{
    // Linux packaging is too varied for an automatic in-place
    // upgrade — distros ship via apt/dnf/AUR/flatpak/etc. and any
    // self-replace path will fight whichever package manager owns
    // /usr/bin. Best UX is to point the user at the release page
    // and let them re-download manually (or update via their
    // package manager when it picks up the new tag).
    Q_UNUSED(tarPath);
    openReleasePage();
    QCoreApplication::quit();
}
#else
void Updater::applyLinux(const QString&) {}
#endif
