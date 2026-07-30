#include "net/SyncLoop.h"
#include "net/MatrixClient.h"

#include <QDebug>
#include <QNetworkInformation>
#include <QRandomGenerator>

SyncLoop::SyncLoop(MatrixClient* client, QObject* parent)
    : QObject(parent)
    , m_client(client)
{
    m_retryTimer.setSingleShot(true);
    connect(&m_retryTimer, &QTimer::timeout, this, &SyncLoop::doSync);

    connect(m_client, &MatrixClient::syncSuccess, this, &SyncLoop::onSyncSuccess);
    connect(m_client, &MatrixClient::syncError, this, &SyncLoop::onSyncError);
}

void SyncLoop::setSince(const QString& since)
{
    m_since = since;
    // A token handed to us from persisted state has never been validated by
    // this server in this session, so it is the one that may need to be
    // abandoned. Tokens we read off a live reply are trusted by definition.
    m_sinceIsResumed = !since.isEmpty();
}

void SyncLoop::start()
{
    if (m_running) return;
    m_running = true;
    m_consecutiveFailures = 0;
    m_noProgressReplies = 0;
    emit runningChanged();
    doSync();
}

void SyncLoop::stop()
{
    if (!m_running) return;
    m_running = false;
    m_retryTimer.stop();
    emit runningChanged();
}

void SyncLoop::scheduleSync(int delayMs)
{
    if (!m_running) return;
    if (delayMs <= 0) {
        doSync();
        return;
    }
    m_retryTimer.start(delayMs);
}

void SyncLoop::doSync()
{
    if (!m_running) return;
    m_requestTimer.start();
    m_client->sync(m_since, 30000);
}

void SyncLoop::onSyncSuccess(const bsfchat::SyncResponse& response)
{
    if (!m_running) return;

    m_consecutiveFailures = 0;
    // The token survived a round trip, so it is no longer a resumed guess.
    m_sinceIsResumed = false;

    const qint64 elapsed = m_requestTimer.isValid()
        ? m_requestTimer.elapsed()
        : static_cast<qint64>(SyncBackoff::kMinSyncIntervalMs);

    const QString previous = m_since;
    m_since = QString::fromStdString(response.next_batch);
    emit syncCompleted(response);
    // A handler is allowed to stop us (disconnect, logout, room teardown).
    if (!m_running) return;

    // /sync is a 30s long poll, so a reply that lands inside the floor means
    // the far end answered without blocking. That is either healthy — events
    // were already queued, and next_batch advanced — or it is an endpoint
    // answering 200 unconditionally: a caching proxy, a reverse proxy
    // serving the wrong upstream, a server bug. This used to re-enter
    // doSync() with no floor at all, which turned the latter into a tight
    // request loop that melts client and server together.
    const bool fast = elapsed < SyncBackoff::kMinSyncIntervalMs;
    const bool progressed = m_since != previous;

    if (fast && !progressed) {
        scheduleSync(SyncBackoff::delayForFailure(
            m_noProgressReplies++,
            QRandomGenerator::global()->generateDouble()));
        return;
    }

    m_noProgressReplies = 0;
    // Real progress: go straight back out, only held to the floor. In a busy
    // room that costs at most kMinSyncIntervalMs of extra latency, which is
    // invisible next to typing speed.
    scheduleSync(fast
        ? static_cast<int>(SyncBackoff::kMinSyncIntervalMs - elapsed)
        : 0);
}

void SyncLoop::onSyncError(const QString& error)
{
    if (!m_running) return;
    emit syncError(error);
    if (!m_running) return;

    // A resumed token the server will not accept must never be retried
    // forever. Drop it on an explicit rejection, and unconditionally once
    // it has burned kMaxResumeAttempts failures — the fallback (a full
    // initial sync) always works, and paying for one beats a client that
    // cannot sync at all.
    if (m_sinceIsResumed
        && (SyncBackoff::indicatesRejectedSinceToken(error)
            || m_consecutiveFailures + 1 >= SyncBackoff::kMaxResumeAttempts)) {
        abandonSinceToken();
    }

    const int delay = SyncBackoff::delayForFailure(
        m_consecutiveFailures++, QRandomGenerator::global()->generateDouble());
    m_retryTimer.start(delay);

    ensureReachabilityWatch();
}

void SyncLoop::abandonSinceToken()
{
    qWarning() << "sync: giving up on the resumed since token"
               << m_since << "— falling back to a full initial sync";
    m_since.clear();
    // Cleared first: the flag is what stops this from firing again and
    // again, which would make every subsequent error re-emit the signal.
    m_sinceIsResumed = false;
    // A different strategy deserves a fresh attempt rather than inheriting
    // the failed one's backoff.
    m_consecutiveFailures = 0;
    emit sinceTokenAbandoned();
}

void SyncLoop::ensureReachabilityWatch()
{
    if (m_reachabilityWatched) return;
    // Set unconditionally: one attempt at loading a platform backend is
    // enough, and retrying it on every error would be its own small waste.
    m_reachabilityWatched = true;

    // Loaded lazily rather than in the constructor so a healthy launch never
    // pays for the platform backend at all — startup cost is the whole point
    // of the surrounding work. By the time we get here we are already in a
    // retry, so a few ms doesn't matter.
    if (!QNetworkInformation::loadDefaultBackend()) return;
    auto* info = QNetworkInformation::instance();
    if (!info) return;

    connect(info, &QNetworkInformation::reachabilityChanged, this,
        [this](QNetworkInformation::Reachability reachability) {
            if (!m_running) return;
            if (reachability != QNetworkInformation::Reachability::Online) return;
            if (!m_retryTimer.isActive()) return;
            // Connectivity came back: don't sit out the remaining 60s of a
            // saturated backoff, and start the schedule over.
            m_retryTimer.stop();
            m_consecutiveFailures = 0;
            doSync();
        });

    // Note: we deliberately do *not* suppress the request while the OS
    // reports "offline". Backoff already keeps a downed server from being
    // hammered, and the reachability backends report Disconnected on a
    // laptop with no WAN — which is exactly the machine talking to a
    // localhost dev server that is perfectly reachable.
}
