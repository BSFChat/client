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
// Security model:
//   * GitHub API + asset URLs over HTTPS only; redirect policy is
//     `NoLessSafeRedirectPolicy` so we don't downgrade to plaintext.
//   * No signature verification yet — TLS is the trust anchor. The
//     long-term move (post-Microsoft-Store enrollment) is to verify
//     the .dmg / .exe code signature before launching the installer.
//   * Downloads land in CacheLocation — a per-user dir the OS will
//     reap on uninstall.
//
// Thread model: every public method runs on the Qt main thread.
// Network I/O is async via QNetworkAccessManager. The platform
// apply paths spawn detached child processes and exit our own
// QCoreApplication so the installer can replace our binary.
#pragma once

#include <QNetworkAccessManager>
#include <QObject>
#include <QString>
#include <QTimer>

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
        AheadOfChannel
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

    // Hand the downloaded artefact to the OS-specific apply path.
    // Spawns the installer / dmg-mount + ditto and exits the app.
    Q_INVOKABLE void applyUpdate();

    // Open the release's GitHub page in the browser. Linux uses this
    // as its only update path.
    Q_INVOKABLE void openReleasePage();

    // Hooked from main() — if the user has auto-check enabled, runs
    // a check 30 seconds after launch (so we don't compete with
    // first-paint / login / sync) and every 6 hours thereafter.
    void startAutoCheckSchedule();

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
    void onDownloadFinished();
    void onDownloadProgress(qint64 received, qint64 total);

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
    QString m_assetLocalPath; // where the downloaded asset lives
    qint64  m_downloadedBytes = 0;
    qint64  m_totalBytes = 0;
    QString m_lastError;
    QString m_latestStableVersion;
    QString m_channel;        // channel used by the most recent check
    QTimer  m_recheckTimer;
};
