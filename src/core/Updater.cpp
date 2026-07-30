#include "Updater.h"

#include "ReleaseSelection.h"

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
#include <QSettings>
#include <QStandardPaths>
#include <QUrl>
#include <QtGlobal>

#ifndef BSFCHAT_VERSION
#  define BSFCHAT_VERSION "0.0.0"
#endif

namespace {

// Version comparison used to live here as a `major.minor.patch` compare
// that threw the `-suffix` away, which made 0.0.44-rc.1 compare EQUAL to
// 0.0.44. It now lives in core/ReleaseSelection.h with real semver
// prerelease precedence, because with RCs published that equality is the
// difference between "here is 0.0.44" and "you are already on it".

// Read the persisted channel without owning a Settings instance.
// Settings uses QSettings("BSFChat", "BSFChat") and sync()s on write, so
// this sees the current value; reading fresh per check also means a
// toggle takes effect on the next check with no wiring between the two
// objects (Updater is constructed in main() before Settings is reachable).
bsfchat::updates::Channel persistedChannel()
{
    QSettings s(QStringLiteral("BSFChat"), QStringLiteral("BSFChat"));
    return bsfchat::updates::channelFromString(
        s.value(QStringLiteral("updateChannel"),
                QStringLiteral("stable")).toString());
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

bool Updater::prereleaseBuild() const
{
    return bsfchat::updates::parseVersion(m_currentVersion).isPrerelease();
}

QString Updater::buildLabel() const
{
    return bsfchat::updates::buildChannelLabel(m_currentVersion);
}

QString Updater::channel() const
{
    return m_channel.isEmpty()
        ? bsfchat::updates::channelToString(persistedChannel())
        : m_channel;
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
    // One GET per check, as before — the list endpoint just returns a
    // bigger body than /releases/latest did. per_page=30 is GitHub's
    // default and is roughly a year of our tagging cadence; we never
    // follow the `Link: rel="next"` page, because a release that has
    // fallen off page one is older than thirty more recent ones and
    // cannot be the newest on any channel. Paginating would also turn
    // one check into N requests against a 60/hour budget.
    QNetworkRequest req(QUrl(QStringLiteral(
        "https://api.github.com/repos/BSFChat/client/releases?per_page=30")));
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
    using namespace bsfchat::updates;

    QString perr;
    const QVector<Release> releases = parseReleaseList(reply->readAll(), &perr);
    if (!perr.isEmpty()) {
        setError(QStringLiteral("Bad JSON from GitHub: %1").arg(perr));
        return;
    }

    const Channel ch = persistedChannel();
    m_channel = channelToString(ch);
    const Selection sel = selectRelease(releases, ch, m_currentVersion);
    m_latestStableVersion = sel.newestStableTag;

    switch (sel.outcome) {
    case Outcome::NoUsableRelease:
        // Every entry was a draft / unclassifiable / unparseable, the
        // repo has no releases, or our own version doesn't parse. We
        // learned nothing, so we say nothing rather than claiming to be
        // current — and there is nothing to download, so this is not an
        // error the user can act on either.
        m_availableVersion.clear();
        setState(Idle);
        return;
    case Outcome::UpToDate:
        m_availableVersion.clear();
        setState(UpToDate);
        return;
    case Outcome::AheadOfChannel:
        // Typically: opted out of beta while running an RC. Product
        // decision is to stay put — no downgrade offer — but this is a
        // different thing from being up to date and is surfaced as such.
        m_availableVersion.clear();
        setState(AheadOfChannel);
        return;
    case Outcome::UpdateAvailable:
        break;
    }

    // Newer tag found. Resolve our platform's asset URL.
    const QString suffix = platformAssetSuffix();
    if (suffix.isEmpty()) {
        // Unsupported platform — leave as Idle so the UI doesn't
        // promise something we can't deliver.
        setState(Idle);
        return;
    }
    const QString matchedUrl = assetUrlNamed(sel.release, suffix);
    if (matchedUrl.isEmpty()) {
        setError(QStringLiteral("Release %1 has no asset named %2")
                 .arg(sel.release.tag, suffix));
        return;
    }

    m_availableVersion = sel.release.tag;
    m_releaseHtmlUrl = sel.release.htmlUrl;
    m_releaseNotes = sel.release.notes;
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
    //   1. waits for our PID to exit (so the bundle is unlocked)
    //   2. attaches the .dmg at a unique mountpoint
    //   3. finds the .app inside the DMG (case-insensitive)
    //   4. asks Finder via AppleScript to duplicate that bundle
    //      into /Applications with replacing. THIS IS DELIBERATE:
    //      on macOS Ventura+, any non-Apple-allowlisted process
    //      that writes to a /Applications/*.app gets blocked by
    //      the App Management TCC. ditto from a detached
    //      bash-child of ours fails silently with "Operation not
    //      permitted" because bsfchat-app has no App Management
    //      grant. Finder is on Apple's permanent allowlist for
    //      this category, so routing the duplicate through Finder
    //      sidesteps the TCC gate without requiring a one-time
    //      user consent dialog.
    //   5. detaches the DMG and relaunches the new app
    //
    // The whole script is wrapped in a logging block so future
    // failures show up in ~/Library/Logs/BSFChat/update.log
    // instead of disappearing into the detached-process void.
    QString appPath = QCoreApplication::applicationDirPath();
    // applicationDirPath = .../bsfchat-app.app/Contents/MacOS — climb to the .app
    QDir d(appPath);
    d.cdUp(); d.cdUp();
    QString bundlePath = d.absolutePath();
    QDir parentDir(bundlePath);
    parentDir.cdUp();
    QString installParent = parentDir.absolutePath();  // typically /Applications

    // Unique mountpoint so a stale leftover or a parallel update
    // attempt can't collide on /Volumes/BSFChatUpdate.
    QString mountPt = QStringLiteral("/Volumes/BSFChatUpdate-%1")
        .arg(QCoreApplication::applicationPid());

    QString script = QStringLiteral(
        "LOG=\"$HOME/Library/Logs/BSFChat/update.log\"; "
        "mkdir -p \"$(dirname \"$LOG\")\"; "
        "exec >>\"$LOG\" 2>&1; "
        "echo; echo \"=== Update started $(date) ===\"; "
        "echo \"  pid_to_wait=%1 mount=%2 dmg=%3 dest=%4 parent=%5\"; "
        "while kill -0 %1 2>/dev/null; do sleep 0.2; done; "
        "echo \"  old process exited, mounting dmg\"; "
        "hdiutil attach -nobrowse -mountpoint %2 %3 || { echo \"ERROR: hdiutil attach failed\"; exit 1; }; "
        "SRC=$(find %2 -maxdepth 2 -iname '*.app' -type d | head -1); "
        "echo \"  src=$SRC\"; "
        "if [ -n \"$SRC\" ]; then "
        // Finder is on Apple's permanent App Management allowlist;
        // routing the duplicate through it sidesteps the TCC gate
        // that blocks our own process from writing to /Applications.
        // `as alias` coerces the POSIX file refs into the form
        // Finder's duplicate verb expects.
        "  osascript -e 'tell application \"Finder\" to duplicate (POSIX file \"'\"$SRC\"'\" as alias) to (POSIX file \"%5\" as alias) with replacing' "
        "    && echo \"  Finder duplicate OK\" "
        "    || echo \"ERROR: Finder duplicate failed (TCC denied?)\"; "
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
        .arg(bundlePath)
        .arg(installParent);

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
