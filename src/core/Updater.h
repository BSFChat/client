// In-app auto-updater for desktop builds.
//
// Polls GitHub's /releases/latest endpoint, compares the published
// `tag_name` against the running build's MACOSX_BUNDLE_SHORT_VERSION_STRING
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

public:
    enum State {
        Idle,           // nothing happening; default
        Checking,       // hitting the GitHub /releases/latest endpoint
        UpToDate,       // we're on the latest tag
        UpdateAvailable,// newer tag found; awaiting user decision
        Downloading,    // user said yes; pulling the asset
        ReadyToApply,   // download done; "Restart to update" enabled
        Applying,       // platform apply path running
        Failed          // network / parse / write error; lastError populated
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
    bool isNewer(const QString& candidate) const;
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
    QTimer  m_recheckTimer;
};
