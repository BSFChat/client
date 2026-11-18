// In-app auto-updater for desktop builds.
//
// Polls GitHub's /releases (list) endpoint, compares the published
// `tag_name`s against the running build's MACOSX_BUNDLE_SHORT_VERSION_STRING
// (passed in at compile time as BSFCHAT_VERSION), and — if newer —
// downloads the platform-specific artefact (.dmg / .exe / .tar.gz)
// to QStandardPaths::CacheLocation. The user is prompted via a QML
// dialog to "Restart to update" or "Later"; "Restart" hands the
// downloaded artefact off to the platform-native apply path:
//
//   macOS    — `hdiutil attach` the .dmg, ditto the .app over the
//              installed bundle, relaunch via QProcess::startDetached.
//   Windows  — start the NSIS installer (it handles process replace +
//              relaunch internally), exit the current process so the
//              installer can write to the locked .exe.
//   Linux    — notification only. Most Linux users get updates via
//              their distro's package manager (or run from the
//              extracted tarball where in-place upgrade isn't safe).
//              We surface "v0.0.X available — download?" with a link
//              to the GitHub release page.
//
// Why GitHub directly instead of a custom appcast feed: we already
// publish releases there, the JSON API is stable, and TLS gives us
// transport integrity for free. No appcast-hosting infrastructure to
// maintain. Downside: 60 requests/hour unauthenticated rate limit
// per IP — fine for our scale (we throttle to one check per 6 hours
// per user anyway).
//
// Release channels:
//   The endpoint is the list (/releases?per_page=30), NOT
//   /releases/latest, because /releases/latest excludes prereleases
//   entirely and no query parameter changes that. Each entry carries a
//   `prerelease` boolean; that flag — never a substring of the tag —
//   decides what a channel may see. Users on "stable" have prereleases
//   filtered out before any comparison; users on "beta" consider both
//   and get whichever is highest under semver precedence. It is still
//   exactly one GET per check, so the rate-limit budget and the 6-hour
//   throttle are untouched; only the response body is larger.
//
//   All of the selection and ordering logic lives in
//   core/ReleaseSelection.h as pure functions, so it is unit-tested
//   without a network (tests/test_update_channel.cpp).
//
//   Opting back out while running an RC does not downgrade: the build
//   stays, and the state becomes AheadOfChannel (not UpToDate) until a
//   stable release exceeds it.
//
// Security model (audit M-5):
//   * Every URL is pinned: the asset link must be an https
//     github.com/BSFChat/client/releases/download/... URL, and every
//     redirect hop must be https on a GitHub release host. Redirects use
//     UserVerifiedRedirectPolicy so each hop is checked before it is
//     followed (core/UpdateIntegrity.h).
//   * TLS is NOT the trust anchor. Each release carries SHA256SUMS, signed
//     by the release CI with an Ed25519 key whose public half is compiled
//     in (core/ReleaseSigningKey.h). The manifest is verified, bound to the
//     release tag, and the installer is hashed while it streams to disk; it
//     only reaches its final path if the digest matches.
//   * A release without SHA256SUMS + .sig is never installed. It is an
//     integrity failure (Failed), unless it is a prerelease that predates
//     release signing (unsignedReleaseVerdict): then the state is
//     UpdateUnsigned and the UI offers its GitHub release page (C2-2).
//   * macOS additionally requires `codesign --verify --deep --strict` on the
//     .app inside the DMG, and the same Team ID as the running build,
//     before the bundle is swapped. Windows Authenticode is not checked:
//     installers are not code-signed today (docs/release-signing.md).
//   * Downloads land in CacheLocation — a per-user dir the OS will
//     reap on uninstall.
//
// Thread model: every public method runs on the Qt main thread.
// Network I/O is async via QNetworkAccessManager. The platform
// apply paths spawn detached child processes and exit our own
// QCoreApplication so the installer can replace our binary.
#pragma once

#include "UpdateIntegrity.h"

#include <QNetworkAccessManager>
#include <QObject>
#include <QString>
#include <QTimer>

#include <functional>

class QNetworkReply;

class Updater : public QObject {
    Q_OBJECT
    Q_PROPERTY(State state READ state NOTIFY stateChanged)
    Q_PROPERTY(QString currentVersion READ currentVersion CONSTANT)
    Q_PROPERTY(QString availableVersion READ availableVersion NOTIFY stateChanged)
    Q_PROPERTY(QString releaseNotes READ releaseNotes NOTIFY stateChanged)
    Q_PROPERTY(qint64 downloadedBytes READ downloadedBytes NOTIFY progressChanged)
    Q_PROPERTY(qint64 totalBytes READ totalBytes NOTIFY progressChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY stateChanged)
    // Newest non-prerelease tag seen on the last check ("" if unknown).
    // Lets the UI say what a build that is ahead of stable is ahead OF.
    Q_PROPERTY(QString latestStableVersion READ latestStableVersion NOTIFY stateChanged)
    // True when the RUNNING build's own version carries a prerelease
    // suffix, independent of the channel setting — a tester who opts out
    // is still running an RC and the UI must keep saying so.
    Q_PROPERTY(bool prereleaseBuild READ prereleaseBuild CONSTANT)
    // Short badge for that build: "RC", "BETA", "DEV", "PRE", or "" for
    // a plain release. Pairs with the existing `currentVersion` display.
    Q_PROPERTY(QString buildLabel READ buildLabel CONSTANT)
    // The channel the last check actually used ("stable" / "beta"),
    // read from the same QSettings key Settings::updateChannel writes.
    Q_PROPERTY(QString channel READ channel NOTIFY stateChanged)

public:
    // QML compares these as bare integers (see ClientSettings.qml and
    // UpdateDialog.qml), so new states MUST be appended, never inserted.
    enum State {
        Idle,           // nothing happening; default
        Checking,       // hitting the GitHub /releases endpoint
        UpToDate,       // we're on the newest tag our channel offers
        UpdateAvailable,// newer tag found; awaiting user decision
        Downloading,    // user said yes; pulling the asset
        ReadyToApply,   // download done; "Restart to update" enabled
        Applying,       // platform apply path running
        Failed,         // network / parse / write error; lastError populated
        // Running a build newer than anything the selected channel
        // publishes — i.e. an RC whose owner has since opted out of
        // beta. Deliberately distinct from UpToDate: nothing is being
        // offered, but "you're up to date" would be a false statement
        // about a build that is ahead of the channel, and the user who
        // just opted out is owed an explanation rather than silence.
        AheadOfChannel,
        // A newer PRERELEASE exists with no signed SHA256SUMS, published
        // before release signing began. It is never downloaded or run; the
        // UI points at its release page. Any other unsigned release is Failed.
        UpdateUnsigned
    };
    Q_ENUM(State)

    explicit Updater(QObject* parent = nullptr);

    State state() const { return m_state; }
    QString currentVersion() const { return m_currentVersion; }
    QString availableVersion() const { return m_availableVersion; }
    QString releaseNotes() const { return m_releaseNotes; }
    qint64 downloadedBytes() const { return m_downloadedBytes; }
    qint64 totalBytes() const { return m_totalBytes; }
    QString lastError() const { return m_lastError; }
    QString latestStableVersion() const { return m_latestStableVersion; }
    bool prereleaseBuild() const;
    QString buildLabel() const;
    QString channel() const;

    // Manual user-triggered check. Goes through the same code path
    // as the auto-check but bypasses the throttle so a user clicking
    // "Check for updates" always hits the network.
    Q_INVOKABLE void checkNow();

    // Download the artefact for the most recently discovered update.
    // No-op if state != UpdateAvailable.
    Q_INVOKABLE void downloadUpdate();

    // Hand the verified artefact to the OS-specific apply path.
    // Spawns the installer / dmg-mount + ditto and exits the app.
    Q_INVOKABLE void applyUpdate();

    // Open the release's GitHub page in the browser. Linux uses this
    // as its only update path, and it is the only path for an
    // UpdateUnsigned release.
    Q_INVOKABLE void openReleasePage();

    // Hooked from main() — if the user has auto-check enabled, runs
    // a check 30 seconds after launch (so we don't compete with
    // first-paint / login / sync) and every 6 hours thereafter.
    void startAutoCheckSchedule();

    // Network inactivity timeouts. checkNow() and downloadUpdate() both
    // refuse to start while a check/download is in flight, so a request
    // that hangs with no timeout wedges the updater for the whole session
    // — including the manual "Check for updates" button. Qt restarts the
    // transfer timer on every received chunk, so the download value is a
    // stall detector, not a deadline on a large artefact.
    static constexpr int kCheckTimeoutMs = 30 * 1000;
    static constexpr int kDownloadStallTimeoutMs = 60 * 1000;
    // GitHub uses one or two hops (github.com -> release-assets...).
    static constexpr int kMaxRedirects = 5;

signals:
    void stateChanged();
    void progressChanged();
    // Convenience signal for QML toast hosts.
    void updateReadyToInstall(QString version);

private:
    void setState(State s);
    void setError(const QString& msg);
    QString platformAssetSuffix() const;

    void onCheckReply();
    void onDownloadProgress(qint64 received, qint64 total);

    // GET with every redirect hop checked against the release-host pin.
    QNetworkReply* pinnedGet(const QUrl& url, int timeoutMs);
    // "" on success, else a user-facing reason (including a refused hop).
    static QString replyFailure(QNetworkReply* reply);
    // Small, capped GET for SHA256SUMS / SHA256SUMS.sig.
    void fetchSmall(const QString& url,
                    std::function<void(const QByteArray&)> onBody);
    void onManifestFetched(const QByteArray& signature);
    void startAssetDownload();
    void failIntegrity(const QString& why);

    void applyMac(const QString& path);
    void applyWindows(const QString& path);
    void applyLinux(const QString& path);

    QNetworkAccessManager m_nam;
    State m_state = Idle;
    QString m_currentVersion;
    QString m_availableVersion;
    QString m_releaseHtmlUrl;
    QString m_releaseNotes;
    QString m_assetUrl;       // direct download URL of the platform asset
    QString m_assetName;      // its name in the release and in SHA256SUMS
    QString m_sumsUrl;        // SHA256SUMS
    QString m_sigUrl;         // SHA256SUMS.sig
    QByteArray m_manifest;    // SHA256SUMS body, unverified until the sig is
    QByteArray m_expectedDigest; // hex SHA-256 of the asset, from a verified manifest
    bsfchat::updates::Sha256FileSink m_sink;
    QString m_assetLocalPath; // where the downloaded asset lives
    qint64  m_downloadedBytes = 0;
    qint64  m_totalBytes = 0;
    QString m_lastError;
    QString m_latestStableVersion;
    QString m_channel;        // channel used by the most recent check
    QTimer  m_recheckTimer;
};
