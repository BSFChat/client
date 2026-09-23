#include "net/SyncLoop.h"
#include "net/MatrixClient.h"

#include <QDebug>
#include <QLoggingCategory>
#include <QNetworkInformation>
#include <QRandomGenerator>

// Per-round-trip /sync diagnostics: how long the poll took, whether the token
// moved, how many events came back, and what we decided to do next.
//
// Off by default like the other bsfchat.* categories — this fires on every
// poll, which is at least twice a minute per client even when nothing is
// happening. Turn it on for a measurement session with either:
//
//   QT_LOGGING_RULES=bsfchat.sync=true ./bsfchat-app
//
// or Settings > Advanced > verbose logging, which sets bsfchat.*=true.
//
// It exists because "messages take ages" is not a measurable claim. Delivery
// lag is the gap between the sender's send and the receiver's next sync
// RETURNING, and only the receiver's client can see the second half of that:
// whether the poll was answered late, or answered promptly with nothing and
// then sat out a backoff.
Q_LOGGING_CATEGORY(logSync, "bsfchat.sync", QtWarningMsg)

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
    // The outstanding poll goes with it. Clearing m_running only made this
    // object IGNORE the reply; the request itself kept running, kept a
    // socket open, and — this is the bug — was still in the air when a
    // re-authentication called start() again moments later. It then landed
    // with m_running back to true, was treated as an ordinary reply, and
    // scheduled its own successor. From then on the connection had two
    // independent poll chains on one token, forever: two requests out, two
    // replies back, each writing m_since over the other's.
    m_client->abortSync();
    m_inFlight = false;
    emit runningChanged();
}

void SyncLoop::refreshNow()
{
    if (!m_running) return;
    m_retryTimer.stop();
    // The outstanding poll is presumed dead, not merely slow — that is the
    // whole premise of being called. Dropping it is what makes the next
    // doSync() actually issue a request rather than be swallowed by the
    // single-flight guard.
    m_client->abortSync();
    m_inFlight = false;
    // Coming back to the foreground is a fresh start, not a continuation of
    // whatever backoff the suspended app had accumulated.
    m_consecutiveFailures = 0;
    m_noProgressReplies = 0;
    doSync();
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
    // Single flight. doSync() is reachable from four places — start(), the
    // retry timer, onSyncSuccess's reschedule and the reachability handler —
    // and the last of those used to fire straight into a poll that was
    // already outstanding. MatrixClient::sync() now supersedes rather than
    // duplicates, so this is belt and braces; it is also the invariant worth
    // stating, because "one /sync per connection" is the property the whole
    // of this class depends on and nothing used to assert it.
    if (m_inFlight) return;
    m_inFlight = true;
    m_requestTimer.start();
    qCDebug(logSync).nospace()
        << "/sync -> since=" << (m_since.isEmpty() ? QStringLiteral("(full)") : m_since)
        << " timeout=" << kSyncTimeoutMs << "ms";
    m_client->sync(m_since, kSyncTimeoutMs);
}

void SyncLoop::onSyncSuccess(const bsfchat::SyncResponse& response)
{
    // Cleared before the m_running gate, not after: a reply that arrives
    // while stopped still ends the flight, or a later start() would find
    // m_inFlight stuck true and never poll again.
    m_inFlight = false;
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

    // Count what the reply actually carried. A long poll can legitimately come
    // back fast without next_batch moving — typing and presence wake it and
    // are not timeline events — so the payload is what tells a healthy server
    // apart from one answering 200 unconditionally. See isNoProgressReply().
    int events = 0;
    int ephemeral = 0;
    for (const auto& [roomId, room] : response.rooms.join) {
        Q_UNUSED(roomId)
        events += static_cast<int>(room.timeline.events.size());
        if (room.ephemeral) ephemeral += static_cast<int>(room.ephemeral->events.size());
    }
    const int presence = response.presence
        ? static_cast<int>(response.presence->events.size()) : 0;
    const bool noProgress =
        SyncBackoff::isNoProgressReply(fast, progressed, events + ephemeral + presence);

    if (logSync().isDebugEnabled()) {
        // A poll that comes back fast with events is healthy (they were
        // already queued). Fast with nothing and no token movement is the
        // shape that costs real latency: it is what triggers the no-progress
        // backoff below, so log the two apart rather than as one "empty".
        qCDebug(logSync).nospace()
            << "/sync rt=" << elapsed << "ms rooms=" << response.rooms.join.size()
            << " events=" << events << " ephemeral=" << ephemeral
            << " presence=" << presence
            << " progressed=" << progressed
            << (noProgress
                    ? QStringLiteral(" NO-PROGRESS (backoff #%1)").arg(m_noProgressReplies)
                    : QString());
    }

    if (noProgress) {
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
    m_inFlight = false;
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
    qCDebug(logSync).nospace()
        << "/sync failed after "
        << (m_requestTimer.isValid() ? m_requestTimer.elapsed() : -1)
        << "ms, retry in " << delay << "ms (failure #" << m_consecutiveFailures
        << "): " << error.left(200);
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
