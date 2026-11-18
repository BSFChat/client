#include "Updater.h"

#include "ReleaseSelection.h"
#include "ReleaseSigningKey.h"
#include "UpdateIntegrity.h"
#include "AppProfile.h"
#include "util/ExternalBrowser.h"

#include <QCoreApplication>
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

#include <functional>
#include <memory>
#include <optional>

#ifndef BSFCHAT_VERSION
#  define BSFCHAT_VERSION "0.0.0"
#endif

using namespace bsfchat::updates;

namespace {

// Version comparison used to live here as a `major.minor.patch` compare
// that threw the `-suffix` away, which made 0.0.44-rc.1 compare EQUAL to
// 0.0.44. It now lives in core/ReleaseSelection.h with real semver
// prerelease precedence, because with RCs published that equality is the
// difference between "here is 0.0.44" and "you are already on it".

// Read the persisted channel without owning a Settings instance.
// Settings sync()s on write, so this sees the current value; reading fresh
// per check also means a toggle takes effect on the next check with no
// wiring between the two objects (Updater is constructed in main() before
// Settings is reachable).
//
// The names MUST come from AppProfile, exactly as Settings builds them.
// Hard-coding ("BSFChat", "BSFChat") here meant that under --profile the
// beta toggle wrote to BSFChat-<profile> while this read BSFChat: the
// switch did nothing, `updater.channel` disagreed with `appSettings`
// in the same dialog, and a profile silently inherited the default
// install's channel.
bsfchat::updates::Channel persistedChannel()
{
    QSettings s(bsfchat::organizationName(), bsfchat::applicationName());
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
    // Without a timeout a request that is accepted and then never answered
    // — a captive portal, a black-holed TCP connection — leaves m_state at
    // Checking forever, and the guard above then makes both the 6-hour
    // timer and the user's own "Check for updates" button permanent no-ops
    // for the rest of the session.
    req.setTransferTimeout(kCheckTimeoutMs);
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
    // Only releases carrying this platform's artefact can be offered. An
    // empty suffix means an unsupported platform: no filter, and the
    // UpdateAvailable branch below bails out to Idle instead of promising
    // a download we cannot perform.
    const QString suffix = platformAssetSuffix();
    const Selection sel = selectRelease(releases, ch, m_currentVersion, suffix);
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

    if (suffix.isEmpty()) {
        // Unsupported platform — leave as Idle so the UI doesn't
        // promise something we can't deliver.
        setState(Idle);
        return;
    }

    // Newer tag found; selectRelease has already guaranteed it carries our
    // platform's asset, so this cannot fail. Kept as a guard rather than an
    // assert because the alternative is offering an install with no file.
    const QString matchedUrl = assetUrlNamed(sel.release, suffix);
    if (matchedUrl.isEmpty()) {
        setError(QStringLiteral("Release %1 has no asset named %2")
                 .arg(sel.release.tag, suffix));
        return;
    }

    // Pin the asset link the API handed us before anything else uses it.
    // This is the JSON body of an unauthenticated API call, so it is not
    // trusted to point anywhere but our own release downloads.
    if (!isAllowedAssetUrl(QUrl(matchedUrl))) {
        setError(QStringLiteral("Release %1 links %2 outside GitHub's release "
                                "downloads; refusing it.")
                 .arg(sel.release.tag, suffix));
        return;
    }

    m_availableVersion = sel.release.tag;
    // Built from the tag rather than taken from the API's html_url, so the
    // one link we ever open is our own repository's release page.
    m_releaseHtmlUrl = QStringLiteral("https://github.com/BSFChat/client/releases/tag/")
                     + QString::fromUtf8(QUrl::toPercentEncoding(sel.release.tag));
    m_releaseNotes = sel.release.notes;
    m_assetUrl = matchedUrl;
    m_assetName = suffix;
    m_sumsUrl = assetUrlNamed(sel.release, QLatin1String(kManifestAssetName));
    m_sigUrl = assetUrlNamed(sel.release, QLatin1String(kSignatureAssetName));

    // No signed manifest. Only a prerelease that predates release signing
    // can legitimately lack one; it gets the release page, never the
    // installer. Anything else had its signature stripped or was never
    // ours: an integrity failure, with no "download it from the website"
    // that would hand the user the very file we refused (C2-2).
    if (m_sumsUrl.isEmpty() || m_sigUrl.isEmpty()) {
        m_sumsUrl.clear();
        m_sigUrl.clear();
        m_assetUrl.clear();
        if (unsignedReleaseVerdict(sel.release, releases,
                                   QString::fromLatin1(kSignaturesRequiredFrom))
            == UnsignedVerdict::OfferReleasePage) {
            setState(UpdateUnsigned);
            return;
        }
        m_releaseHtmlUrl.clear();
        m_availableVersion.clear();
        m_releaseNotes.clear();
        setError(QStringLiteral("BSFChat %1 could not be verified and was not "
                                "offered: the release carries no signed "
                                "checksums.").arg(sel.release.tag));
        return;
    }
    if (!isAllowedAssetUrl(QUrl(m_sumsUrl)) || !isAllowedAssetUrl(QUrl(m_sigUrl))) {
        setError(QStringLiteral("Release %1 links its checksums outside GitHub's "
                                "release downloads; refusing it.")
                 .arg(sel.release.tag));
        return;
    }
    setState(UpdateAvailable);
}

namespace {
// Set on a reply we aborted ourselves, so the finished handler can say why
// instead of reporting "Operation canceled".
constexpr char kRefusedProp[] = "bsfchatUpdaterRefused";
} // namespace

QNetworkReply* Updater::pinnedGet(const QUrl& url, int timeoutMs)
{
    QNetworkRequest req(url);
    // UserVerified: Qt stops at every 3xx and waits for redirectAllowed(),
    // so each hop's scheme and host are checked BEFORE it is followed.
    // NoLessSafeRedirectPolicy (the old setting) only refused https->http
    // and would follow a redirect to any https host.
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::UserVerifiedRedirectPolicy);
    req.setMaximumRedirectsAllowed(kMaxRedirects);
    req.setRawHeader("User-Agent", "BSFChat-Updater");
    // Inactivity timeout, not a deadline: Qt restarts it on every chunk, so
    // a slow but progressing 120 MB download is unaffected while a stalled
    // one no longer pins the state machine at Downloading forever.
    req.setTransferTimeout(timeoutMs);
    QNetworkReply* reply = m_nam.get(req);
    connect(reply, &QNetworkReply::redirected, reply, [reply](const QUrl& to) {
        const QUrl target = to.isRelative() ? reply->url().resolved(to) : to;
        if (isAllowedRedirectUrl(target)) {
            emit reply->redirectAllowed();
            return;
        }
        reply->setProperty(kRefusedProp, QStringLiteral(
            "refused a redirect to %1://%2").arg(target.scheme(), target.host()));
        reply->abort();
    });
    return reply;
}

QString Updater::replyFailure(QNetworkReply* reply)
{
    const QVariant refused = reply->property(kRefusedProp);
    if (refused.isValid()) return refused.toString();
    if (reply->error() != QNetworkReply::NoError) return reply->errorString();
    return QString();
}

void Updater::failIntegrity(const QString& why)
{
    m_sink.abort();
    // No pointer to the release page: if the bytes we fetched from it
    // were tampered with, so is the file a browser would download (C2-2).
    setError(QStringLiteral("The update failed its integrity check and was "
                            "not installed (%1).").arg(why));
}

void Updater::fetchSmall(const QString& url,
                         std::function<void(const QByteArray&)> onBody)
{
    QNetworkReply* reply = pinnedGet(QUrl(url), kCheckTimeoutMs);
    connect(reply, &QNetworkReply::downloadProgress, reply,
            [reply](qint64 received, qint64) {
        if (received > kMaxManifestBytes) {
            reply->setProperty(kRefusedProp, QStringLiteral("checksum file too large"));
            reply->abort();
        }
    });
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, url, onBody = std::move(onBody)]() {
        reply->deleteLater();
        if (m_state != Downloading) return;
        const QString why = replyFailure(reply);
        if (!why.isEmpty()) {
            setError(QStringLiteral("Couldn't fetch %1: %2")
                     .arg(QFileInfo(QUrl(url).path()).fileName(), why));
            return;
        }
        const QByteArray body = reply->read(kMaxManifestBytes + 1);
        if (body.size() > kMaxManifestBytes) {
            failIntegrity(QStringLiteral("checksum file too large"));
            return;
        }
        onBody(body);
    });
}

void Updater::downloadUpdate()
{
    if (m_state != UpdateAvailable) return;
    // UpdateAvailable is only ever set with all three URLs pinned; this is
    // the belt to that brace, so nothing unsigned can reach the installer.
    if (m_assetUrl.isEmpty() || m_sumsUrl.isEmpty() || m_sigUrl.isEmpty()) return;

    m_downloadedBytes = 0;
    m_totalBytes = 0;
    m_manifest.clear();
    m_expectedDigest.clear();
    m_assetLocalPath.clear();
    setState(Downloading);

    // SHA256SUMS, then its signature, then — only once both verify and
    // name this asset — the asset itself.
    fetchSmall(m_sumsUrl, [this](const QByteArray& manifest) {
        m_manifest = manifest;
        fetchSmall(m_sigUrl, [this](const QByteArray& sig) {
            onManifestFetched(sig);
        });
    });
}

void Updater::onManifestFetched(const QByteArray& signature)
{
    QString why;
    const std::optional<QByteArray> digest = expectedAssetDigest(
        m_manifest, signature, m_availableVersion, m_assetName,
        releaseSigningKeys(), &why);
    if (!digest) {
        failIntegrity(why);
        return;
    }
    m_expectedDigest = *digest;
    startAssetDownload();
}

void Updater::startAssetDownload()
{
    // Cache under updates/<asset name>. The sink writes to a temp file and
    // only replaces this path once the hash has matched, so a failed or
    // tampered download never leaves a runnable file behind.
    const QString finalPath = cacheDir() + "/" + m_assetName;
    QString why;
    if (!m_sink.open(finalPath, &why)) {
        setError(QStringLiteral("Can't write to %1: %2").arg(finalPath, why));
        return;
    }

    QNetworkReply* reply = pinnedGet(QUrl(m_assetUrl), kDownloadStallTimeoutMs);
    connect(reply, &QNetworkReply::downloadProgress,
            this, &Updater::onDownloadProgress);
    // Stream to disk chunk by chunk; the installer is never held in RAM
    // (it used to be one readAll() of the whole artefact, audit M-12).
    connect(reply, &QNetworkReply::readyRead, this, [this, reply]() {
        if (!m_sink.write(reply->readAll())) {
            reply->setProperty(kRefusedProp, QStringLiteral(
                "couldn't save the update: %1").arg(m_sink.errorString()));
            reply->abort();
        }
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, finalPath]() {
        reply->deleteLater();
        if (m_state != Downloading) { m_sink.abort(); return; }
        const QString failure = replyFailure(reply);
        if (!failure.isEmpty()) {
            m_sink.abort();
            setError(QStringLiteral("Download failed: %1").arg(failure));
            return;
        }
        // Anything still buffered after the last readyRead.
        if (!m_sink.write(reply->readAll())) {
            const QString err = m_sink.errorString();
            m_sink.abort();
            setError(QStringLiteral("Couldn't save the update to %1: %2")
                     .arg(finalPath, err));
            return;
        }
        QString mismatch;
        if (!m_sink.finish(m_expectedDigest, &mismatch)) {
            failIntegrity(mismatch);
            return;
        }
        m_assetLocalPath = finalPath;
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
    // ReadyToApply is only reached through Sha256FileSink::finish(); a path
    // that did not come from there is never handed to an installer.
    if (m_assetLocalPath.isEmpty() || m_expectedDigest.isEmpty()) return;
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
    // openExternalUrl logs the refusal; there is no in-app surface for this
    // one to report to, and unlike a sign-in nothing is left half-finished
    // by it — but the log line is the difference between "the button is
    // broken" and a diagnosable machine.
    bsfchat::openExternalUrl(QUrl(m_releaseHtmlUrl));
}

#if defined(Q_OS_MACOS)
// $1 pid to wait for, $2 mount point (already attached and verified by
// applyMac), $3 the verified .app inside it, $4 installed bundle, $5 its
// parent directory. See applyMac for why these are arguments, not text.
static const char kMacUpdateScript[] = R"SH(
LOG="$HOME/Library/Logs/BSFChat/update.log"
mkdir -p "$(dirname "$LOG")"
exec >>"$LOG" 2>&1
echo; echo "=== Update started $(date) ==="
echo "  pid_to_wait=$1 mount=$2 src=$3 dest=$4 parent=$5"
while kill -0 "$1" 2>/dev/null; do sleep 0.2; done
echo "  old process exited"
# Finder is on Apple's permanent App Management allowlist; routing the
# duplicate through it sidesteps the TCC gate that blocks our own process
# from writing to /Applications. `as alias` coerces the POSIX file refs
# into the form Finder's duplicate verb expects.
osascript -e 'on run argv' \
          -e 'tell application "Finder" to duplicate (POSIX file (item 1 of argv) as alias) to (POSIX file (item 2 of argv) as alias) with replacing' \
          -e 'end run' "$3" "$5" \
  && echo "  Finder duplicate OK" \
  || echo "ERROR: Finder duplicate failed (TCC denied?)"
hdiutil detach "$2" -force
echo "  relaunching $4"
open "$4"
echo "=== Update done $(date) ==="
)SH";

namespace {

// Run a tool asynchronously and hand back (exited 0, merged output). The
// verification steps below take seconds on a 100 MB bundle, so they must
// not block the GUI thread.
void runTool(QObject* ctx, const QString& program, const QStringList& args,
             int timeoutMs, std::function<void(bool, const QString&)> done)
{
    auto* proc = new QProcess(ctx);
    proc->setProcessChannelMode(QProcess::MergedChannels);
    auto fired = std::make_shared<bool>(false);
    auto finish = [proc, fired, done](bool ok, const QString& out) {
        if (*fired) return;
        *fired = true;
        proc->deleteLater();
        done(ok, out);
    };
    QObject::connect(proc, &QProcess::finished, ctx,
                     [proc, finish](int code, QProcess::ExitStatus st) {
        finish(st == QProcess::NormalExit && code == 0,
               QString::fromUtf8(proc->readAll()));
    });
    QObject::connect(proc, &QProcess::errorOccurred, ctx,
                     [program, finish](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart)
            finish(false, QStringLiteral("couldn't start %1").arg(program));
    });
    QTimer::singleShot(timeoutMs, proc, [proc]() { proc->kill(); });
    proc->start(program, args);
}

QString lastLine(const QString& out)
{
    const QStringList lines = out.trimmed().split(QLatin1Char('\n'));
    return lines.isEmpty() ? QString() : lines.last().trimmed();
}

} // namespace

void Updater::applyMac(const QString& dmgPath)
{
    // Before the running app quits, in this process:
    //   1. attach the (hash-verified) .dmg read-only at a unique mountpoint
    //   2. find the one .app at its top level
    //   3. `codesign --verify --deep --strict` it, and require the same
    //      Team ID as the build that is running (or BSFCHAT_MACOS_TEAM_ID)
    // Any failure detaches the image and reports in the update UI, with the
    // current app still running. Only then is a detached shell helper
    // spawned that:
    //   4. waits for our PID to exit (so the bundle is unlocked)
    //   5. asks Finder via AppleScript to duplicate the verified bundle
    //      into the install directory with replacing. THIS IS DELIBERATE:
    //      on macOS Ventura+, any non-Apple-allowlisted process that
    //      writes to a /Applications/*.app gets blocked by the App
    //      Management TCC. ditto from a detached bash-child of ours fails
    //      silently with "Operation not permitted" because bsfchat-app
    //      has no App Management grant. Finder is on Apple's permanent
    //      allowlist for this category, so routing the duplicate through
    //      Finder sidesteps the TCC gate without requiring a one-time
    //      user consent dialog.
    //   6. detaches the DMG and relaunches the new app
    //
    // The script logs to ~/Library/Logs/BSFChat/update.log instead of
    // disappearing into the detached-process void.
    QString appPath = QCoreApplication::applicationDirPath();
    // applicationDirPath = .../bsfchat-app.app/Contents/MacOS — climb to the .app
    QDir d(appPath);
    d.cdUp(); d.cdUp();
    const QString bundlePath = d.absolutePath();
    QDir parentDir(bundlePath);
    parentDir.cdUp();
    const QString installParent = parentDir.absolutePath();  // typically /Applications

    // Unique mountpoint so a stale leftover or a parallel update
    // attempt can't collide on /Volumes/BSFChatUpdate.
    const QString mountPt = QStringLiteral("/Volumes/BSFChatUpdate-%1")
        .arg(QCoreApplication::applicationPid());

    const auto fail = [this, mountPt](const QString& why) {
        QProcess::startDetached(QStringLiteral("/usr/bin/hdiutil"),
                                {QStringLiteral("detach"), mountPt,
                                 QStringLiteral("-force")});
        setError(QStringLiteral("The update was not installed: %1").arg(why));
    };

    const QString codesign = QStringLiteral("/usr/bin/codesign");
    const auto spawnInstaller = [this, mountPt, bundlePath, installParent](const QString& src) {
        // The paths are passed as POSITIONAL ARGUMENTS ($1..$5) and quoted
        // at every use, never spliced into the script text. They used to be
        // interpolated with chained .arg() — unquoted into hdiutil/open, and
        // inside an AppleScript string literal — so an app at
        // "/Applications/BSFChat Beta.app", a home directory with a space,
        // or a '"' anywhere split the arguments and the update aborted
        // AFTER the old app had already quit. The AppleScript gets its
        // paths through `on run argv` for the same reason. kMacUpdateScript
        // is a plain constant so the whole script can be exercised off macOS.
        const bool started = QProcess::startDetached(
            QStringLiteral("/bin/bash"),
            {QStringLiteral("-c"), QString::fromUtf8(kMacUpdateScript),
             QStringLiteral("bsfchat-update"),
             QString::number(QCoreApplication::applicationPid()), mountPt,
             src, bundlePath, installParent});
        if (!started) {
            QProcess::startDetached(QStringLiteral("/usr/bin/hdiutil"),
                                    {QStringLiteral("detach"), mountPt,
                                     QStringLiteral("-force")});
            setError(QStringLiteral("Couldn't start the installer helper."));
            return;
        }
        QCoreApplication::quit();
    };

    // 3b. Team ID: the new bundle must be signed by the same team as the
    // running one. A build-time BSFCHAT_MACOS_TEAM_ID overrides; with
    // neither (an ad-hoc local build), strict verification alone applies.
    const auto checkTeam = [this, fail, codesign, bundlePath, spawnInstaller](
                               const QString& src, const QString& newTeam) {
#ifdef BSFCHAT_MACOS_TEAM_ID
        Q_UNUSED(codesign);
        Q_UNUSED(bundlePath);
        const QString pinned = QStringLiteral(BSFCHAT_MACOS_TEAM_ID);
        if (newTeam != pinned) {
            fail(QStringLiteral("it is signed by team \"%1\", expected %2")
                 .arg(newTeam, pinned));
            return;
        }
        spawnInstaller(src);
#else
        runTool(this, codesign,
                {QStringLiteral("-d"), QStringLiteral("--verbose=2"), bundlePath},
                30 * 1000,
                [fail, src, newTeam, spawnInstaller](bool, const QString& out) {
            const QString selfTeam = bsfchat::updates::parseCodesignTeamId(out);
            if (!selfTeam.isEmpty() && newTeam != selfTeam) {
                fail(QStringLiteral("it is signed by team \"%1\", not %2 like "
                                    "this build").arg(newTeam, selfTeam));
                return;
            }
            if (selfTeam.isEmpty())
                qWarning("Updater: running build has no Team ID; "
                         "relying on codesign --verify only");
            spawnInstaller(src);
        });
#endif
    };

    runTool(this, QStringLiteral("/usr/bin/hdiutil"),
            {QStringLiteral("attach"), QStringLiteral("-nobrowse"),
             QStringLiteral("-noautoopen"), QStringLiteral("-readonly"),
             QStringLiteral("-mountpoint"), mountPt, dmgPath},
            120 * 1000,
            [this, fail, codesign, mountPt, checkTeam](bool ok, const QString& out) {
        if (!ok) {
            setError(QStringLiteral("Couldn't open the update disk image: %1")
                     .arg(lastLine(out)));
            return;
        }
        const QStringList apps = QDir(mountPt).entryList(
            {QStringLiteral("*.app")}, QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks);
        if (apps.size() != 1) {
            fail(QStringLiteral("expected one app in the disk image, found %1")
                 .arg(apps.size()));
            return;
        }
        const QString src = mountPt + QLatin1Char('/') + apps.first();
        runTool(this, codesign,
                {QStringLiteral("--verify"), QStringLiteral("--deep"),
                 QStringLiteral("--strict"), QStringLiteral("--verbose=2"), src},
                180 * 1000,
                [this, fail, codesign, src, checkTeam](bool verified, const QString& vout) {
            if (!verified) {
                fail(QStringLiteral("its code signature is invalid (%1)")
                     .arg(lastLine(vout)));
                return;
            }
            runTool(this, codesign,
                    {QStringLiteral("-d"), QStringLiteral("--verbose=2"), src},
                    30 * 1000,
                    [fail, src, checkTeam](bool, const QString& dout) {
                const QString team = bsfchat::updates::parseCodesignTeamId(dout);
                checkTeam(src, team);
            });
        });
    });
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
    //
    // Integrity: exePath only exists once its SHA-256 matched the signed
    // SHA256SUMS (see startAssetDownload). Authenticode is NOT checked
    // here: the installers are not code-signed today (the CI signing step
    // is skipped without WINDOWS_CERT_PFX_BASE64; v0.0.48-rc.19's .exe has
    // no certificate table). Add a WinVerifyTrust check once they are.
    QProcess::startDetached(exePath, {"/S"});
    QCoreApplication::quit();
}
#else
void Updater::applyWindows(const QString&) {}
#endif

#if defined(Q_OS_LINUX)
void Updater::applyLinux(const QString& tarPath)
{
    // Linux packaging is too varied for an automatic in-place upgrade —
    // distros ship via apt/dnf/AUR/flatpak/etc. and any self-replace path
    // will fight whichever package manager owns /usr/bin. So the user
    // installs it, but from the tarball we just verified against the signed
    // SHA256SUMS: its folder is opened. It used to open the release page,
    // i.e. send the user to re-download an UNverified copy (C2-7).
    // The app keeps running; nothing has been replaced.
    const QString dir = QFileInfo(tarPath).absolutePath();
    setState(ReadyToApply);
    if (!bsfchat::openExternalUrl(QUrl::fromLocalFile(dir))) {
        setError(QStringLiteral("Couldn't open a file manager. The verified "
                                "download is at %1").arg(tarPath));
    }
}
#else
void Updater::applyLinux(const QString&) {}
#endif
