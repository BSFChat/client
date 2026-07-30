#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QTimer>
#include <bsfchat/MatrixTypes.h>

#include "net/SyncBackoff.h"

class MatrixClient;

class SyncLoop : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool running READ isRunning NOTIFY runningChanged)

public:
    explicit SyncLoop(MatrixClient* client, QObject* parent = nullptr);

    void start();
    void stop();
    bool isRunning() const { return m_running; }

    // Seed the loop's stream position from persisted state so a launch can
    // resume incrementally instead of paying for a full initial sync. Must
    // be called before start(); an empty token means "full sync".
    void setSince(const QString& since);
    QString since() const { return m_since; }

    // Consecutive-failure counter driving the backoff. Exposed for the
    // connection-status UI and for tests.
    int consecutiveFailures() const { return m_consecutiveFailures; }

signals:
    void syncCompleted(const bsfchat::SyncResponse& response);
    void syncError(const QString& error);
    void runningChanged();
    // Emitted when a *resumed* `since` token has been given up on and the
    // loop has fallen back to a full initial sync. The owner must drop its
    // persisted copy, otherwise the next launch retries the same bad token.
    void sinceTokenAbandoned();

private:
    void doSync();
    void scheduleSync(int delayMs);
    void onSyncSuccess(const bsfchat::SyncResponse& response);
    void onSyncError(const QString& error);
    void abandonSinceToken();
    void ensureReachabilityWatch();

    MatrixClient* m_client;
    QTimer m_retryTimer;
    // Measures how long the in-flight /sync took, which is how a reply that
    // was answered without blocking is told apart from a long poll that
    // legitimately had events waiting.
    QElapsedTimer m_requestTimer;
    QString m_since;
    bool m_running = false;
    int m_consecutiveFailures = 0;
    // Successive fast replies that did not advance next_batch. Escalated on
    // the same curve as errors so a broken 200 cannot spin.
    int m_noProgressReplies = 0;
    // True while m_since came from persisted state rather than from a reply
    // we saw this session — the only window in which discarding it and
    // full-syncing is the correct recovery.
    bool m_sinceIsResumed = false;
    bool m_reachabilityWatched = false;
};
